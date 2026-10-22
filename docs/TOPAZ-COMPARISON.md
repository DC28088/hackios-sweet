# Topaz reference for Sweet

Reference: [MakrSas/ios-topaz](https://github.com/MakrSas/ios-topaz/tree/e4772911e116ab4ab6c4ac1f38e6106fefe8b483), reviewed 2026-10-22.
Preserve its APSL/MIT attribution when importing code; see its CREDITS.md.

## GPU first

Both ports expose HackiOS's RedwoodGPU msm-style client to Mesa freedreno and
interpose EAGL for CoreAnimation. Sweet's prepared APFS helper already contains
this userspace stack, built for its matching iOS16.5 shared cache. Topaz's
injection addresses target iOS16.7.16 and cannot replace these addresses.

| Layer | Topaz A610 | Sweet A618 |
| --- | --- | --- |
| Power | Direct GX and clocks; no GMU | GMU firmware, legacy HFI and OOB power votes |
| GPU aperture | 0x05900000 | 0x05000000 |
| GPUCC | 0x05990000 | 0x05090000 |
| GPU SMMU | 0x059a0000 | 0x05040000 |
| UBWC highest bank bit | 13 | 14 |
| Kernel client | RedwoodGPU | RedwoodGPU |
| Renderer | Mesa freedreno / EAGL | Mesa freedreno / EAGL |

The comparison exposed an existing Sweet bug: `redwood_gpu_parameter(0x10)`
returned13 although `core_setup()` programmed the A618 hardware for14.
Corrected to14, consistent with Linux `a6xx_calc_ubwc_config()`'s A618 case.
This is a concrete layout fix; it does not establish successful command
execution, correct rendering, or a frame-rate improvement.

Topaz's useful reusable areas include its GPU memory ownership/cache handling,
IOSurface sharing and frame timing instrumentation. Evaluate these against the
actual Sweet failure stage. First obtain GMU/HFI startup, SQE/CP fence completion,
RedwoodGPU registration and the Mesa GL_RENDERER output from the same boot.
A successful CP fence alone does not prove CoreAnimation is using the GPU.
Do not replace A618's GMU code with Topaz's GMU-less startup sequence.

## Wi-Fi

Topaz patches0113–0130 implement modem PAS boot, GLINK/QRTR host services,
WLFW, copy engines, HTC/WMI/HTT and WPA2 station handling. Patch0900 adapts
HackiOS's Apple80211 frontend to that transport. The frontend/packet plumbing
is reusable; the modem bootstrap and CE/SMMU programming require Sweet data.

Topaz's0123 uses WLAN0x0c800000, SMMU0x0c600000, stream0x1a0 and
SMEM0x46000000. The public Sweet DTS instead describes ICNSS0x18800000,
stream0x240 and SMEM0x86000000, with its own MSA reservation and regulators.
These addresses are not interchangeable. Keep `WIFI_ATH11K=0` until the proper
Sweet transport is adapted; enabling Redwood's driver does not supply it.
Use the device's own firmware and calibration privately, never commit them.

## Current boot prerequisite

Pinned Sweet UEFI selects a host-forcing `UsbConfigDxe.patched.efi`. Firmware
preparation now selects the original same-pin binary, guarded by SHA256, so
ConfigUsbEvent can select peripheral mode and bind UsbfnDwc3Dxe. This is the
resident loader's USB path; it is separate from Topaz's in-kernel CDC-ECM code.
