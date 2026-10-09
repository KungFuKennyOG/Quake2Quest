# SoF SDK 64-bit port

`sof-sdk-64bit.patch` makes the game code of Raven's Soldier of Fortune SDK (`Source/Game`)
build as 64-bit Linux/Android shared libraries (gcc/clang): portable replacements for MSVC-only
code, LP64 fixes in save games and file formats, and `extern "C"` exports.

Apply it from the SDK's `Source` directory:

    patch -p1 < sof-sdk-64bit.patch

`Makefile.linux` builds `gamex64.so` and `player.so` for desktop testing
(copy it next to the patched `Game` directory and run `make`).
The Android build uses `Projects/Android/jni/quake2/Android_sof.mk`.
