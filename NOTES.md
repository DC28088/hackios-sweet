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
