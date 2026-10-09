#!/bin/sh
# build the headless SoF host. SDK=<path to the ported SoF SDK "Game" dir>, OUT=<output dir>
set -e
SDK=${SDK:?set SDK to the ported SDK Game directory}
OUT=${OUT:-.}
SRC=$(cd "$(dirname "$0")/../../../Projects/Android/jni/quake2/src" && pwd)
G=$SRC/sof/ghoul
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT/obj"
for f in $HERE/cm_shim.c $HERE/pm_shim.c $SRC/common/pmove.c $SRC/common/collision.c $SRC/common/crc.c $SRC/common/md4.c $SRC/common/shared/shared.c $SRC/common/shared/rand.c $SRC/common/shared/flash.c; do
  gcc -c -O1 -g -w -DDEDICATED_ONLY -I$SRC -I$HERE -DYQ2OSTYPE='"Linux"' -DYQ2ARCH='"x86_64"' -o "$OUT/obj/$(basename $f).o" $f
done
g++ -std=gnu++11 -O1 -g -w -I$SDK/gamecpp -I$SDK/qcommon -I$SDK/ghoul -I$G -I$HERE -include $SDK/qcommon/port_compat.h \
  -o "$OUT/sofhost" $HERE/sofhost.cpp $G/ghoul_runtime.cpp $G/ghoul_gsq.cpp $G/ghb_model.cpp $G/ghb_dirtable.cpp \
  $SDK/ghoul/matrix4.cpp $SDK/ghoul/vect3.cpp "$OUT"/obj/*.o -ldl
echo built "$OUT/sofhost"
