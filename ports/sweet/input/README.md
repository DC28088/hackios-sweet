# Sweet input migration

Hardware sources are reconstructed from the ordered patches in
`DC28088/ios-sweet`, commit `fe987bf05e0ddadd77433c2812018e6937df355a`.
Their original comments are retained. The original repository has no top-level
license file; this directory does not assign a new license to those sources.

The HakiOS target remains pinned to
`88513176b1e96ecf0b764cd62c6f4e6ac71345e3`.

- `sweet_touch.c`: SM7150 SE7 GENI I2C, Goodix GT9896 Yellowstone or
  FocalTech 54:52. Coordinates are physical 1080 by 2400.
- `sweet_keys.c`: PM6150/PM6150L SPMI observer reads for power and volume.
  PMIC configuration is left to firmware.
- `sweet_rpmh.c`: command DB and apps RSC implementation, including the
  touch L11C regulator vote. Later standalone SMMU probe and DDR startup
  functions are excluded because this input port does not invoke them.
- `hakios.sweet_input.c`: converts the original hardware callbacks into
  HakiOS queue records, including DOWN/MOVE/UP state and normalized coordinates.

Startup is gated by `sweet=1`, replacing Redwood touch/button initialization
on this board. `sweettouch=0` and `sweetkeys=0` disable individual drivers.
The original 45-second touch startup delay remains configurable through
`sweettouchdelay`. `touch_lat=1` enables queue event timestamps in live logs.

This migration does not claim working hardware until a matching kernel is
booted and touch/button events are observed and confirmed on the phone.
