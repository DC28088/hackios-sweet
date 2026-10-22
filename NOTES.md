# HackiOS Sweet handoff

2026-10-22: user changed direction after reporting Sweet0116 booted with zero
performance improvement. Requests a new repository porting HackiOS to Sweet
and a compiled boot image. Repository: https://github.com/DC28088/hackios-sweet.
Working tree /home/dan/hackios-sweet. Original /home/dan/ios-sweet is preserved.

Upstream kernel8e61187098f2c26c5daee8f9f72b6416aa64b9ab, userland906425b75e02b4f99eb5d7bb1ef7c9583472d772.
Both source trees combined here; see upstream-lock.json and LICENSE.userland.
Sweet UEFI pin098e303fd9703669ab06d38744cad14d49c0f702 (tagicmi Mu tree).

Initial source c84a685: Sweet loader/GIC/CPU/display profile, no raw logdump
writes, Sweet UEFI payload reservation + handoff, independent BootShim,
A618 firmware/clock/legacy GMU/HFI adaptation retaining HackiOS RedwoodGPU
MSM DRM service and Mesa/EAGL frontend. Build39771804932 is running kernel
and firmware jobs. No tests, phone commands or device writes performed.

Hardware port is NOT complete/qualified. Pending source review: parent rail
votes, A618 core register profile/GMEM and cleanup, Sweet I2C touchscreen (the
upstream touch driver is SPI), buttons, CPU frequency and USB adaptation.
Current default Sweet bootargs omit touchscreen/cpufreq/wifi/adsp. Need correct
Sweet input before a usable iOS boot. Preserve evidence and explicit gaps.

The boot image design is HackiOS's resident USB loader, with the compiled XNU
artifact separate. iOS16.5 APFS system/root images must be prepared separately;
the existing iOS16.7.16 HFS+ userdata cannot serve as that system image. Do not
reuse the old injected sweetgl.dylib with this kernel. The GLES build here is
the HackiOS original /redwood/lib frontend, not the previous NativeAdreno ABI.

Local vendor firmware exists in /home/dan/ios-sweet/research/gpu/device-firmware
(a618_gmu.bin, a630_sqe.fw, a615_zap.*); never commit it. Sweet live FDT is
/home/dan/ios-sweet/backup/live-fdt.dtb; use it read-only for public board facts.

Board adaptation continued: imported Sweet's existing GENI I2C touch (FocalTech/
GT9896), PMIC keys and serialized RPMh driver from ios-sweet through0114.
Adapted their output to HackiOS /dev/redwood-touch; no old HID service reused.
GPU rail prerequisites now use that same RPMh owner; A618 GMEM window programmed.
Initial CI39771804932 failed before kernel compilation (exported headers) and
loaderCOFF fixup.39772127622 identifies ADRP block_buffer+1MiB addend and SMC
assembler feature check. Replaced constant-addend pointer with opaque base,
SMC encoding with identical architectural .inst. Kernel export log now emitted
on failure. GLES39772020663 still building at time of this entry.

GLES CI39772020663 SUCCESS; artifacts downloaded out/gles-39772020663:
libOSMesa.8.dylib, libredwood-gl.dylib and backboardd/SpringBoard libraries.
Source2048b0f. No runtime test. Kernel39772464704 failed on missing host
uuid/uuid.h before exporthdrs; added uuid-dev. Firmware still running.
Latest work also closes direct write_logs partition paths (not only flush_log),
adds bounded touch-power synchronization and A618 register protection table.

GLES files locally checksum/ARM64 validated. libOSMesa.8.dylib14822264bytes
SHA25615b698258864d0ef4294d7d426d55cfb360edce1fb5aabfddc045b2dc5da2b19;
libredwood-gl.dylib541896bytes SHA25615de976fbd8a796ec455710b6855c0290908a63ac9212077e0e6823fa4a59861.
New strict Sweet packager ports/sweet/package.py validates Sweet FD size,
ARM64 BootShim/PE, DTB magic; no fallback to upstream Redwood prebuilt pieces.
Loader in USB mode now refuses rootless one-shot boot if USB setup fails.
No device commands performed. Firmware39772464704 still installing dependencies.

Runtime scripts adapted for the combined repository: common/setup.sh no longer
creates a second userland checkout over tracked files; xl requires out/loader
and out/kernel Sweet artifacts and defaults to Sweet bootargs. No Redwood
prebuilt fallback. Root preparation still needed for iOS16.5; host free space
~4.8GiB is below upstream extraction's ~8GB requirement, so no download begun.

A618 review caught two further A660 assumptions before hardware boot: SQE
needs CP_RB_RPTR_ADDR plus CP_WHERE_AM_I memory shadow; ring_send now emits
it, reads invalidate/acquire the dedicated scratch line, submit reserves3
extra words. Core GPU_SET OOB uses legacy request16/ack24, and shutdown uses
legacy BOOT_SLUMBER request22/ack30, not the uninitialized modern HFI table.
Latest kernel build is required; earlier source artifacts are superseded.

CI39773154339: kernel and firmware jobs both SUCCESS. Overall run failed only
because the separate loader job lacked lld-link; that was corrected and loader
CI39773276597 onaca7259 SUCCEEDED. Final A618 SQE/cleanup kernel source659167c
build39773636216 is running; it supersedes the earlier compiled kernel.
Artifacts downloading: firmware out/firmware and initial kernel
out/kernel-initial-3febfc8. Loader already in out/loader. Userland in
out/gles-39772020663. Package workflow added to produce downloadable boot image
from explicit component run IDs. No tests or device access.
