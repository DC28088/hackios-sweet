#!/bin/bash
# Build the reviewed current EAGL/injection bridge against our verified Mesa library.
# No programs or hardware probes are run.
set -euo pipefail
source ./xnu-env.sh
UPSTREAM=${1:?upstream source directory}
OUTPUT=$PWD/out/current-gles
mkdir -p "$OUTPUT" "$UPSTREAM/userland/obj"
MESA=$OUTPUT/libOSMesa.8.dylib
printf '%s  %s\n' 15b698258864d0ef4294d7d426d55cfb360edce1fb5aabfddc045b2dc5da2b19 "$MESA" | sha256sum -c -
"$XNUDEPS/host/bin/nm" -gU "$MESA" | awk '{print $NF}' | sed -n 's/^_\(gl[A-Za-z0-9_]*\)$/\1/p' | sort -u > "$OUTPUT/exported-gl.txt"
test -s "$OUTPUT/exported-gl.txt"
curl -fL --retry 3 https://raw.githubusercontent.com/KhronosGroup/OpenGL-Registry/6af574a14089ccfee87efe230ebcdd8742859813/xml/gl.xml -o "$OUTPUT/gl.xml"
python3 "$UPSTREAM/userland/mkeagl.py" "$UPSTREAM/userland/src/eagl_slots.txt" "$OUTPUT/gl.xml" "$OUTPUT/exported-gl.txt" "$UPSTREAM/userland/src/eagl.c" > "$UPSTREAM/userland/obj/eagl_gen.h"
DYN=(xnu-clang -arch arm64 -isysroot "$THEOS/sdks/iPhoneOS16.5.sdk" -O1)
"${DYN[@]}" -dynamiclib -install_name /redwood/lib/libhakios.gl.dylib -I"$UPSTREAM/userland/obj" -framework CoreFoundation -framework IOKit -framework IOSurface -lobjc "$MESA" -o "$OUTPUT/libhakios.gl.dylib" "$UPSTREAM/userland/src/eagl.c" "$UPSTREAM/userland/src/gl_osmesa.c"
"${DYN[@]}" -dynamiclib -install_name /redwood/lib/libhakios.dylib -framework CoreFoundation -framework IOKit -lobjc -o "$OUTPUT/libhakios.dylib" "$UPSTREAM/userland/src/inject.c"
"${DYN[@]}" -dynamiclib -install_name /redwood/lib/libhakios.sb.dylib -framework CoreFoundation -framework IOKit -lobjc -o "$OUTPUT/libhakios.sb.dylib" "$UPSTREAM/userland/src/inject_sb.c"
git -C "$UPSTREAM" rev-parse HEAD > "$OUTPUT/UPSTREAM_COMMIT"
printf '%s\n' 'Mesa reused from CI39772020663 (softpipe + freedreno); current bridge source88513176. This does not qualify A618 hardware acceleration.' > "$OUTPUT/PROVENANCE"
(cd "$OUTPUT" && sha256sum *.dylib > SHA256SUMS)
