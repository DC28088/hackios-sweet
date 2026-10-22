#!/bin/bash
# Compile HackiOS's original Mesa/EAGL path. Does not run diagnostic programs.
set -euo pipefail
source ./xnu-env.sh
DRIVERS=softpipe,freedreno bash tools/mk-mesa.sh
M=$XNUDEPS/build/mesa/out
O=$PWD/out/userland
mkdir -p "$O" "$PWD/userland/obj"
python3 userland/mkeagl.py userland/src/eagl_slots.txt "$M/gl.xml" "$M/exported-gl.txt" userland/src/eagl.c > userland/obj/eagl_gen.h
DYN=(xnu-clang -arch arm64 -isysroot "$THEOS/sdks/iPhoneOS16.5.sdk" -O1)
"${DYN[@]}" -dynamiclib -install_name /redwood/lib/libredwood-gl.dylib -Iuserland/obj -framework CoreFoundation -framework IOKit -framework IOSurface -lobjc "$M/libOSMesa.8.dylib" -o "$O/libredwood-gl.dylib" userland/src/eagl.c userland/src/gl_osmesa.c
"${DYN[@]}" -dynamiclib -install_name /redwood/lib/libredwood.dylib -framework CoreFoundation -framework IOKit -lobjc -o "$O/libredwood.dylib" userland/src/inject.c
"${DYN[@]}" -dynamiclib -install_name /redwood/lib/libredwood-sb.dylib -framework CoreFoundation -framework IOKit -lobjc -o "$O/libredwood-sb.dylib" userland/src/inject_sb.c
cp "$M/libOSMesa.8.dylib" "$O/"
cp upstream-lock.json "$O/"
(cd "$O" && sha256sum *.dylib > SHA256SUMS)
