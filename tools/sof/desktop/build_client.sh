#!/bin/bash
# Build the headless desktop client (full engine: client + server, no video/sound/input/VR).
#   OUT=<output binary> [OBJDIR=<dir to keep objects>] [ASAN=1] ./build_client.sh
# Run it from the game data directory, e.g.
#   LD_PRELOAD=$(gcc -print-file-name=libasan.so) RUN_MS=20000 ./q2cl +set game sof +map tut1
# (LD_PRELOAD is needed when the game adapter was built with AddressSanitizer.)
set -e
OUT=${OUT:-q2cl}
HERE=$(cd "$(dirname "$0")" && pwd)
Q=$(cd "$HERE/../../../Projects/Android/jni" && pwd)
S=$Q/quake2/src
T=${OBJDIR:-$(mktemp -d)}
mkdir -p "$T"
SAN=""
[ -n "$ASAN" ] && SAN="-fsanitize=address"
FILES="backends/generic/misc.c client/cl_cin.c client/cl_console.c client/cl_download.c
client/cl_effects.c client/cl_entities.c client/cl_input.c client/cl_inventory.c client/cl_keyboard.c
client/cl_lights.c client/cl_main.c client/cl_network.c client/cl_parse.c client/cl_particles.c
client/cl_prediction.c client/cl_screen.c client/cl_tempentities.c client/cl_view.c client/menu/menu.c
client/menu/qmenu.c client/menu/videomenu.c common/argproc.c common/clientserver.c common/collision.c
common/crc.c common/cmdparser.c common/cvar.c common/filesystem.c common/glob.c common/md4.c
common/movemsg.c common/frame.c common/netchan.c common/pmove.c common/szone.c common/zone.c
common/shared/flash.c common/shared/rand.c common/shared/shared.c common/unzip/ioapi.c
common/unzip/miniz.c common/unzip/unzip.c server/sv_cmd.c server/sv_conless.c server/sv_entities.c
server/sv_game.c server/sv_init.c server/sv_main.c server/sv_save.c server/sv_send.c server/sv_user.c
server/sv_world.c backends/unix/network.c backends/unix/signalhandler.c
backends/unix/system.c backends/unix/shared/hunk.c"
CF=(-c -g -O1 -w -fno-omit-frame-pointer $SAN -DIOAPI_NO_64 -DYQUAKE2 -DSOF_BUILD
    '-DBASEDIRNAME="SoFQuest"' '-DYQ2OSTYPE="Linux"' '-DYQ2ARCH="x86_64"'
    -I"$HERE/shim" -I"$Q/quake2" -I"$S" -I"$Q/Quake2VR" -idirafter "$Q/SupportLibs/gl4es/include")
pids=()
for f in $FILES; do
	gcc "${CF[@]}" -o "$T/$(echo $f | tr / _ | sed 's/\.c$/.o/')" "$S/$f" & pids+=($!)
done
gcc "${CF[@]}" -o "$T/headless_client.o" "$HERE/headless_client.c" & pids+=($!)
gcc "${CF[@]}" -o "$T/matrixlib.o" "$Q/Quake2VR/matrixlib.c" & pids+=($!)
for p in "${pids[@]}"; do wait $p; done
gcc $SAN -rdynamic -o "$OUT" "$T"/*.o -lm -ldl -lpthread /lib/x86_64-linux-gnu/libEGL.so.1
[ -n "$OBJDIR" ] || rm -rf "$T"
echo built "$OUT"
