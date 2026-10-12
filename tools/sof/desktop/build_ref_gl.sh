#!/bin/sh
# Build the real GL1 renderer against the desktop's OpenGL (Mesa), so the headless client
# can draw real frames offscreen and save them as pictures:
#   OUT=ref_gl.so ./build_ref_gl.sh
#   SOF_GL=1 SOF_REF=$PWD/ref_gl.so SOF_SHOT=shot SOF_SHOT_FRAMES=200,400 ./q2cl +map tsr1
# (writes shot_200.ppm, shot_400.ppm; the frames are 1280x720, one eye, no lens distortion)
set -e
OUT=${OUT:-ref_gl.so}
HERE=$(cd "$(dirname "$0")" && pwd)
Q=$(cd "$HERE/../../../Projects/Android/jni" && pwd)
S=$Q/quake2/src
T=${OBJDIR:-$(mktemp -d)}
mkdir -p "$T"
CF="-c -g -O1 -fPIC -w -DUSE_GLES1 -DYQ2OSTYPE=\"Linux\" -DYQ2ARCH=\"x86_64\" -I$HERE/shim -I$Q/quake2 -I$Q/SupportLibs/gl4es/include -I$Q/SupportLibs/gl4es"
pids=""
for f in client/refresh/gl1/qgl.c client/refresh/gl1/gl1_draw.c client/refresh/gl1/gl1_image.c \
         client/refresh/gl1/gl1_light.c client/refresh/gl1/gl1_lightmap.c client/refresh/gl1/gl1_main.c \
         client/refresh/gl1/gl1_mesh.c client/refresh/gl1/gl1_misc.c client/refresh/gl1/gl1_model.c \
         client/refresh/gl1/gl1_scrap.c client/refresh/gl1/gl1_surf.c client/refresh/gl1/gl1_warp.c \
         client/refresh/gl1/gl1_sdl.c client/refresh/gl1/gl1_md2.c client/refresh/gl1/gl1_sp2.c \
         client/refresh/gl1/gl1_sof.c client/refresh/files/pcx.c client/refresh/files/stb.c \
         client/refresh/files/wal.c client/refresh/files/pvs.c common/shared/shared.c common/md4.c \
         backends/unix/shared/hunk.c
do
	eval gcc $CF -o "$T/$(echo $f | tr / _ | sed 's/\.c$/.o/')" "$S/$f" &
	pids="$pids $!"
done
gcc -c -g -O1 -fPIC -o "$T/gl_desktop.o" "$HERE/gl_desktop.c" & pids="$pids $!"
for p in $pids; do wait $p; done
gcc -shared -Wl,-Bsymbolic -o "$OUT" "$T"/*.o /lib/x86_64-linux-gnu/libGL.so.1 -lm
[ -n "$OBJDIR" ] || rm -rf "$T"
echo built "$OUT"
