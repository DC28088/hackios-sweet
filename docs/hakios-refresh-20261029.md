# Sweet migration to current HakiOS

The original `/home/dan/ios-sweet` installation remains the working baseline.
This experiment uses RAM boot and the existing iOS16.5 APFS system image;
it does not migrate or replace the iOS16.7.16 HFS+ userdata filesystem.

## Source references reviewed

| Repository | Revision | Useful evidence / limits |
| --- | --- | --- |
| [HakiOS kernel](https://github.com/oosexgang/hakios-xnu-kernel) | 88513176b1e96ecf0b764cd62c6f4e6ac71345e3 | Contains current userland and generic loader interfaces. Adreno initialization still includes Redwood-specific registers/firmware. |
| [Native Metal](https://github.com/oosexgang/hakios-metal) | 54a46fadec5bd9684ebd77f3a0834c84b5eb01a6 | Copy/fill commands and completion work on Redwood; README says shader execution is unfinished. UI rendering currently uses Mesa GLES. |
| [HakiOS UEFI](https://github.com/oosexgang/hakios-uefi) | 687fa6c6a5a70f43184de92980e0a76eea5f2c4b | Shared infrastructure; Sweet still needs its own platform, memory map and vendor drivers. |
| [Redwood / SM7325](https://github.com/oosexgang/hakios-sm7325) | 04fbfff933f52faf203a55aca9dd5680092e6ec5 | Device-specific resources differ from SM7150; do not reuse the default peripheral arguments. |
| [Poco X3 Pro / SM8150](https://github.com/nomorecoolnicknames/hakios-sm8150) | 3e7be137a100cef19f1d7e83346b6e688be697d1 | Separate A640 driver, Mesa integration, partial audio/main camera. Current README distinguishes older hardware results from newly compiled, unbooted tree; haptics unverified. |
| [UniXNU Sweet](https://github.com/CrackerCat/UniXNU-sweet) | README reviewed 2026-10-29 | Explicitly reports no bootable image yet; Darwin23/4K baseline differs from this port. |
| [Lisa](https://github.com/Abhi-Flex1/ios-lisa) | README reviewed 2026-10-29 | Does not substantiate the reported smooth hardware rendering; may not be the repository the user meant. |

Pixel5 HFS+/camera reference has not yet been identified. Search results do not
establish a successful current HakiOS boot on Sweet beyond our own experiment.

## Boot qualification

The first attempt used verified upstream prebuilt kernel 03e794e8e1d389a6...
and a separate helper with current init/injection libraries. USB loader and
2.4GB APFS RAM upload succeeded, then the kernel launch was acknowledged but
no kernel log returned. User reported UEFI text followed by reset. This is
not a successful XNU or iOS boot. Logs are `logs/074146-run.log.*` (ignored).

The next loader restores upstream console muting during USB mode. This removes
our old ConOut calls during handoff, a possible stall source, not yet a proven
cause. Source builds use XNU's `build` target; `all` also tries to package Apple
KEXT exports missing from this non-Apple configuration, after linking a kernel.

Do not enable `gpu=1`, `adreno=`, `display_probe=`, Redwood Wi-Fi/audio/camera,
or UFS writes on an unadapted current kernel. A618 needs its own firmware,
legacy HFI/power/clock configuration, SMMU mapping and command submission wired
into the current HakiosGPU userspace interface. The original working port's
Wi-Fi/BT/battery changes need corresponding integration; they do not transfer
just by changing the kernel image.
