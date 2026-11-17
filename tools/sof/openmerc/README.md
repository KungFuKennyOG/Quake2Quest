# SoF SDK game code on the openMERC engine

[openMERC](https://github.com/sidreshot/openMERC) (GPL v2) is an engine for Soldier of Fortune built on
Yamagi Quake II. Its "mode A" hosts the original game and player modules. The SDK game code from
`tools/sof/sdk` (patched with `sof-sdk-64bit.patch`) plugs into it as a native 64-bit module:

- the module tables match SoF 1.07f: the patch adds the 1.07f `ClientPreConnect` export slot (left NULL),
  returns `ReadGame`'s result as an int, and adds the one inventory method the 1.07f table has
  before `getCurWeaponType` (the engine calls the inventory by slot number);
- the shared records (entity, player state, entity prefix, trace, user command, movement block,
  import table) already have the same 64-bit layout on both sides.

Desktop check (Linux x86_64), as done on 2026-11-17:

1. Build openMERC: `tools/build/build-linux.sh x86_64 Release` (needs SDL 3, OpenAL Soft, CMake 3.31+).
2. Build the SDK modules: apply the patch, `make -f Makefile.linux` → `gamex64.so`, `player.so`.
3. Game folder: `base/pak0.pak …` (your SoF data), the openMERC binaries next to `base/`, and
   `openmerc/` holding `zz_openmerc.pak`, `openmerc-portable.txt`, and the SDK modules renamed
   `gamex86.so` and `player.so`.
4. `./openMerc-ded +map tut1` (server only) or `./openMerc +map tut1`. Do not run as root.

Result: tut1 loads and runs on both; the tutorial script runs, weapons are given, selected and fired,
and the view weapon and effects draw.
