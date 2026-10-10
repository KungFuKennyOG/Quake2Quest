# Playing SoF Quest

## Setup

1. Install the APK from the latest "SoF Quest APK" GitHub Actions run (artifact `SoFQuest-apk`).
2. Copy `pak0.pak` … `pak3.pak` from `Soldier of Fortune\Base` into `/sdcard/SoFQuest/` on the headset.
   (Leave mod paks such as sofplus / basicpack out.)
3. Launch "SoF Quest" from Library → Unknown Sources.

The first level is set in `/sdcard/SoFQuest/commandline.txt` (created on first launch):

```
quake2 +set developer 1 +map tut1
```

Change `tut1` to another map name (e.g. `trn1`) to start elsewhere. There are no SoF menus yet.

## Controls (right-handed)

| Right controller | |
|---|---|
| Trigger | Fire (shots go where the controller points) |
| Grip | Alternate fire |
| A | Crouch (or crouch for real) |
| B | Jump |
| Stick left / right | Turn |
| Stick up / down | Previous / next weapon |
| Stick click | Laser sight on/off |

| Left controller | |
|---|---|
| Stick | Move |
| Stick click | Use (doors, buttons, switches) |
| Trigger | Run |
| Grip (hands close together) | Steady a two-handed weapon |
| X | Help / objectives |
| Y | Reload |
| Menu button | Quick save |

## Saving

- **Menu button** quick saves ("Quick save" shows on screen).
- When you die, pressing fire reloads the quick save, or the start of the level if there is none.
- The game also saves automatically when a level starts (`save0`).
- Console: `save <name>` / `load <name>`, `quicksave` / `quickload`.

## Settings

- `vr_worldscale` (default 36 for SoF): game units per metre. Raise it if the world feels too big.
- `sof_vraim 0`: aim with the head instead of the controller.
- `sof_giveall 1`: start every level with all weapons (SoF's own weapon cheat), e.g.
  `quake2 +set developer 1 +set sof_giveall 1 +map tut1` in `commandline.txt`.
