# Soldier of Fortune on Quake2Quest — build notes

This branch can build a second app, **SoF Quest**, that runs Raven's *Soldier of Fortune*
(2000) game code on the Quake2Quest engine. You need your own copy of the game.

## How it fits together

| Part | Where | Notes |
|---|---|---|
| Engine (Yamagi Quake II + Quake2Quest VR) | `Projects/Android/jni/quake2/src` | BSP v46 maps, M32 textures, GHOUL drawing hooks |
| GHOUL runtime (clean-room) | `src/sof/ghoul` | loads `.ghb` models, animation, bolts, hit tests |
| Adapter (a Quake 2 game module) | `src/sof/adapter` | hosts the SoF game module, mirrors its entities for the engine |
| SoF game + player modules | the SoF SDK (not in this repo) | 64-bit port patch applied |

## Build

1. Get the SoF SDK sources (`Source/Game` from the Raven SoF SDK) and apply the 64-bit
   port patch from the parent of `Game`:

       cd sof-sdk/Source
       patch -p1 < sof-sdk-64bit.patch

2. Build the APK with the SDK path:

       cd Projects/Android
       ./gradlew assembleRelease -PsofSdk=/path/to/sof-sdk/Source/Game

   Without `-PsofSdk` the project builds the normal Quake2Quest.

The SoF build installs as `com.drbeef.quake2quest.sof` ("SoF Quest"), next to Quake2Quest.

## Game data

Copy the `.pak` files from your *Soldier of Fortune\Base* folder (`pak0.pak` … `pak3.pak`)
to `/sdcard/SoFQuest/` on the headset. Mod paks (SoFPlus etc.) are not needed.

## Desktop testing (Linux, no GPU)

`tools/sof/desktop` builds the whole engine headless (video/sound/VR stubbed) and the adapter,
so maps, the game logic and the GHOUL draw lists can be checked without a headset:

    SDK=/path/to/Game OUT=Quake2Quest/game.so tools/sof/desktop/build_adapter.sh

`tools/sof/sofhost` is a smaller harness that runs the SoF game module directly.

## Status

Working (tested headless): maps, collision, the SoF game logic and AI, player movement,
cinematic cameras, GHOUL models and the view weapon reaching the renderer.

Not done yet: SoF's own HUD, effects (muzzle flashes, blood, decals), dynamic texture damage,
save games, menus beyond the Quake2Quest ones, VR tuning of the SoF weapons.
