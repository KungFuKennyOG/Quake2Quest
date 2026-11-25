# SoF Quest on openMERC

This branch is [openMERC](https://github.com/sidreshot/openMERC) (GPL v2, Soldier of Fortune engine on Yamagi
Quake II) with the Quest port added on top. It runs the game code of Raven's SoF SDK, built natively for arm64,
in place of openMERC's own game and player modules (like openMERC's "mode A", which runs the original modules).

| Path | What |
|---|---|
| `quest/build-android.sh` | arm64-v8a build: SDL 3 + OpenAL Soft, the OpenXR loader, engine (`libmain.so`), `ref_gles3`, `OMsnd`, the SDK modules, APK |
| `quest/sdk/` | the SDK port: `sof-sdk-64bit.patch` (64-bit + the 1.07f module tables) and its `Makefile` |
| `quest/AndroidManifest.xml` | the APK manifest (package `com.kungfukenny.sofquest`) |
| `.github/workflows/sof-openmerc-quest.yml` | CI: builds the APK on every push to `sof-openmerc` |

## State

VR through OpenXR (`engine/src/client/vr/`, after Dr. Beef's Quake2Quest): the head is the camera, one picture
per eye, the gun is in the hand and shots leave along the controller, snap turn, steps in the room move the
player, menus and films on a screen standing in the room, the HUD floating with the head.
`vr_sim 1` runs the same code on a desktop with a headset made of numbers (`vr_sim.c`).

To do: the scope, HUD on the gun and wrist, weapon wheel, the fit of each weapon's barrel to the hand
(`vr_weapon_turn "yaw pitch"`, `vr_weapon_offset`, `vr_weaponscale` to try values).

## Install

1. `adb install -r SoFQuest.apk`
2. Copy your Soldier of Fortune `base` folder (with `pak0.pak` ...) to
   `/sdcard/Android/data/com.kungfukenny.sofquest/files/base` (start the app once first so the folder exists).
3. Start "SoF Quest" from the Library (Unknown Sources).

## Controls (right handed; `vr_control_scheme 10` swaps the hands)

| | Gun hand | Off hand |
|---|---|---|
| Trigger | fire | walk / run |
| Grip | alternate fire | next item (two hands close together: steady the gun) |
| Stick | left/right turn, up/down next/previous weapon | move |
| Stick press | use item | use / open |
| A / X | crouch | scores |
| B / Y | jump | reload |
| Menu button | | menu (Escape) |

In menus: point with the gun hand and pull the trigger; sticks move, A accepts, B goes back.

Settings (console): `vr_snapturn_angle` (45; 10 or less turns smoothly), `vr_height_adjust`, `vr_worldscale` (36),
`vr_weapon_pitchadjust` (-20), `vr_supersampling` (1.0), `vr_refresh` (72), `vr_hud_size`, `vr_hud_depth`,
`vr_screen_size`, `vr_screen_depth`, `vr_walkdirection` (1 head, 0 off hand), `vr_enable 0` (flat window).

## Desktop check

`tools/build/build-linux.sh` builds openMERC for Linux; build the SDK modules with
`make -C <SDK>/Source -f quest/sdk/Makefile` and put them in `openmerc/` as `gamex86.so` and `player.so`
(see the README of `tools/sof/openmerc` on the `sof-bsp46-m32` branch).
