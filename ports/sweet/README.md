# Sweet port build and boot layout

## What is ported

- Pinned Sweet Mu UEFI (SM7150), payload reservation and direct HackiOS handoff.
- ARM64 relocation shim and board-specific loader profile: GICv3, eight CPUs,
  six little cores, 1080x2400 framebuffer. Loader logs remain in RAM/USB.
- GENI I2C FocalTech/GT9896 input and PMIC keys feeding HackiOS's input protocol.
- Shared RPMh owner for touch and GPU rails; A618 clocks, flat GMU firmware,
  legacy HFI v1, GPU OOB handshakes, SMMU aperture, signed ZAP reservation,
  GMEM window, register protections and SQE read-pointer shadow.
- HackiOS's RedwoodGPU MSM DRM client and original Mesa25 freedreno/EAGL
  frontend, built together for iOS16.5. No previous NativeAdreno service or
  sweetgl.dylib is part of this userspace interface.

These are compiled source adaptations. No hardware boot or compositor
rendering result is available yet. Wi-Fi, audio, camera and CPU frequency
control have not been ported; their Redwood boot options remain off.

## Build artifacts

The `HackiOS Sweet` workflow compiles XNU, UEFI and the loader. The
`HackiOS Sweet GLES` workflow compiles libOSMesa and the EAGL/injection
libraries. `Package HackiOS Sweet image` takes explicit run IDs and produces
`hackios-sweet-boot.img` with component hashes and source revisions.

The image contains the Sweet firmware, resident USB loader and compiled XNU.
It does **not** contain Apple's iOS system files. With USB available the
loader waits for host commands and the APFS images. If USB startup fails,
it reports the failure and does not attempt an incomplete rootless boot.
Packaging binds the generic Sweet DTB's board identity to the saved phone's
public board ID46 (Sweetin); other variants remain unqualified.

Only Sweet artifacts in `out/firmware`, `out/loader` and `out/kernel` are
accepted by the launch path. Old Redwood prebuilt images are not substitutes.

## System image preparation

HackiOS expects the iPhone X iOS16.5/20F66 root and cryptexes as APFS images.
Its original extraction/build procedure is in `ios165/fetch.sh` and
`userland/build.sh`. From the repository root, run:

```sh
bash ports/sweet/prepare-root.sh
```

Allow 12 GiB free for extraction, compressed output and its temporary spool.
The wrapper downloads iOS16.5 directly from Apple's CDN, verifies the compiled
GLES artifact and private Sweet firmware, and assembles `userland/root.apfs`
and `userland/system.apfs`. Override `GLES_DIR` or `GPU_FW_DIR` for another
local artifact directory. The firmware directory needs its checksum manifest.
The default GLES artifact is run39772020663; its source lock must match this
repository. Image hashes and metadata go in `out/root/` after both images
finish. The helper includes GLES and its backboardd/SpringBoard injection
libraries, with no Wi-Fi credentials or SSH private keys. This command does
not communicate with the phone. Apple images and vendor firmware stay local.

The installed ios-sweet16.7.16 HFS+ userdata is a different root layout. It
was left untouched throughout this port. Booting this kernel against it is
not an installation method. The former `/usr/lib/sweetgl.dylib` uses another
kernel ABI and must not be copied into HackiOS.

For a full HackiOS session, prepare the APFS images with `GL=1`, make the
compiled Sweet GPU firmware files available to the image builder locally,
and upload the system image into RAM through the resident loader. Vendor
firmware is never committed. Use `common/run.sh sweet` only after those
images and Sweet artifacts have been prepared. That script boots temporarily;
there is no reason to flash recovery, bootloaders or userdata for bring-up.
