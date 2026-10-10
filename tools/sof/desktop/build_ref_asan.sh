#!/bin/sh
# Build the real GL1 renderer for desktop testing: AddressSanitizer on, OpenGL replaced
# by gl_stub.c (draws nothing, but reads every buffer the renderer hands to GL).
# OUT=<output .so>   then run the headless client with
#   LD_PRELOAD=$(gcc -print-file-name=libasan.so) SOF_REF=<OUT> [SOF_TEST_DLIGHT=1] ./q2cl +map trn1
set -e
OUT=${OUT:-ref_gl1_asan.so}
HERE=$(cd "$(dirname "$0")" && pwd)
Q=$(cd "$HERE/../../../Projects/Android/jni" && pwd)
S=$Q/quake2/src
T=${OBJDIR:-$(mktemp -d)}   # OBJDIR=<dir> keeps the objects (to swap in instrumented copies)
mkdir -p "$T"
CF="-c -g -O1 -fPIC -w -fsanitize=address -fno-omit-frame-pointer -DUSE_GLES1 -DYQ2OSTYPE=\"Linux\" -DYQ2ARCH=\"x86_64\" -I$HERE/shim -I$Q/quake2 -I$Q/SupportLibs/gl4es/include -I$Q/SupportLibs/gl4es"
for f in client/refresh/gl1/qgl.c client/refresh/gl1/gl1_draw.c client/refresh/gl1/gl1_image.c \
         client/refresh/gl1/gl1_light.c client/refresh/gl1/gl1_lightmap.c client/refresh/gl1/gl1_main.c \
         client/refresh/gl1/gl1_mesh.c client/refresh/gl1/gl1_misc.c client/refresh/gl1/gl1_model.c \
         client/refresh/gl1/gl1_scrap.c client/refresh/gl1/gl1_surf.c client/refresh/gl1/gl1_warp.c \
         client/refresh/gl1/gl1_sdl.c client/refresh/gl1/gl1_md2.c client/refresh/gl1/gl1_sp2.c \
         client/refresh/gl1/gl1_sof.c client/refresh/files/pcx.c client/refresh/files/stb.c \
         client/refresh/files/wal.c client/refresh/files/pvs.c common/shared/shared.c common/md4.c \
         backends/unix/shared/hunk.c
do
	eval gcc $CF -o "$T/$(echo $f | tr / _ | sed 's/\.c$/.o/')" "$S/$f"
done
gcc -c -g -O1 -fPIC -fsanitize=address -o "$T/gl_stub.o" "$HERE/gl_stub.c"
gcc -shared -fsanitize=address -o "$OUT" "$T"/*.o -lm
[ -n "$OBJDIR" ] || rm -rf "$T"
echo built "$OUT"
