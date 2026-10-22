#!/bin/bash
# Boot HackiOS on a device whose images are built (common/build.sh <device>).
#
# usage: common/run.sh <device> [seconds]
#   common/run.sh qemu-virt          QEMU window, 20 minutes (mouse = finger, H = home, L = lock)
#   common/run.sh redwood 900        the phone over USB: loader in RAM, system image in RAM, 15 minutes
#
# Nothing is flashed: on a phone everything lives in RAM and `./xl reboot` (or a power-key reset)
# brings back Android. The session's log is printed (QEMU) or saved in logs/ (phones).
. "$(dirname "$(readlink -f "$0")")/lib.sh"
load_device "$1"
SECS=${2:-$DEFAULT_SECS}
mkdir -p logs
[ -f userland/root.apfs ] && [ -f userland/system.apfs ] || die "no images: run common/build.sh $DEVICE first"

case "$KIND" in
qemu)
	QEMU_SMP=${QEMU_SMP:-$SMP} CMDLINE="${CMDLINE:-$BOOTARGS}" qemu/run-ios.sh "$SECS" -display "${DISPLAY_KIND:-gtk}" |
		tee logs/qemu-$(date +%H%M%S).log
	;;
phone)
	command -v fastboot >/dev/null || die "fastboot is not installed"
	SESSION_LOG="logs/session-$(date +%Y%m%d-%H%M%S).log"
	exec > >(tee -a "$SESSION_LOG") 2>&1
	echo "run.sh: host transcript: $SESSION_LOG"
	# Save snapshots while the loader remains on its own core (bootcpu=N).
	export XL_FOLLOW=${XL_FOLLOW:-5}
	export UEFI_TARGET FD_BASE FD_SIZE PAYLOAD_BASE UEFI_FD DTB
	./xl enter || die "could not get the phone into the loader (USB? unlocked bootloader?)"
	./xl ramdisk userland/system.apfs || die "system image upload failed"
	XL_SECS=$SECS XL_CMDLINE="${XL_CMDLINE:-$BOOTARGS}" ./xl run || die "kernel session failed; inspect logs/"
	echo "run: './xl screenshot shot.png' for the screen, './xl reboot' to go back to Android"
	;;
*) die "unknown KIND=$KIND in common/devices/$DEVICE.conf" ;;
esac
