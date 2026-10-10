#!/bin/sh
# Build the SoF adapter (game.so for the engine) on Linux for desktop testing.
# SDK=<ported SoF SDK Game dir>  OUT=<output .so>
set -e
SDK=${SDK:?}
OUT=${OUT:-game.so}
SRC=$(cd "$(dirname "$0")/../../../Projects/Android/jni/quake2/src" && pwd)
T=$(mktemp -d)
gcc -c -O1 -g -fPIC -w -DYQ2OSTYPE='"Linux"' -DYQ2ARCH='"x86_64"' -o $T/q2side.o $SRC/sof/adapter/q2side.c
g++ -shared -fPIC -std=gnu++11 -O1 -g -w -I$SDK/gamecpp -I$SDK/qcommon -I$SDK/ghoul -I$SRC/sof/ghoul -I$SRC/sof/adapter \
  -include $SDK/qcommon/port_compat.h -o "$OUT" $T/q2side.o $SRC/sof/adapter/sofside.cpp \
  $SRC/sof/ghoul/ghoul_runtime.cpp $SRC/sof/fx/sof_fx.cpp $SRC/sof/ghoul/ghoul_gsq.cpp $SRC/sof/ghoul/ghb_model.cpp $SRC/sof/ghoul/ghb_dirtable.cpp \
  $SDK/ghoul/matrix4.cpp $SDK/ghoul/vect3.cpp $SDK/gamecpp/q_sh_fx.cpp -ldl
rm -rf $T
echo built "$OUT"
