# Survive - Wilderness survival — Nintendo Switch port

A wrapper/port of the Android release of **Survive - Wilderness survival 2.880**
(Sandbay Games, `com.sandbaygames.survive`) for the Nintendo Switch. It loads
the game's own arm64 libraries (`libmain.so`, `libunity.so`, `libil2cpp.so` —
Unity 2022.3.62f2, IL2CPP), resolves their imports against native Switch code
and patches them so the game runs in a minimal Android environment.

No game code or assets are included. You need your own copy of the APK.

## Installation

1. Create `/switch/survive_nx/` on the SD card and put `survive_nx.nro` in it.
2. Open the **Survive - Wilderness survival 2.880** APK (arm64) with 7-Zip or
   any other ZIP tool and extract **everything** into the same folder:

```text
/switch/survive_nx/
  survive_nx.nro
  lib/arm64-v8a/libmain.so
  lib/arm64-v8a/libunity.so
  lib/arm64-v8a/libil2cpp.so
  assets/bin/Data/...
  AndroidManifest.xml, res/, META-INF/ ...   (removed by the port at first launch)
```

3. Start it with a **game override** (hold R while starting a game) or a
   forwarder with full memory. The album (applet mode) does not have enough
   memory.

The first launch takes a while: the port checks the files, removes the Android
files it does not use and packs the assets into one file for faster loading.
When it says *Game data is ready*, press A and start the port again.

Optional: a `cursor.png` (up to 64×64, transparency supported) in the folder
replaces the cursor.

## Controls

| Input | Action |
| --- | --- |
| Touchscreen | the game's own touch controls (multitouch, handheld) |
| Left stick | movement (a finger on the on-screen joystick) |
| Right stick | look around (a finger dragged over the screen) |
| ZL | show / hide the cursor |
| Right stick (cursor shown) | move the cursor |
| A / ZR (cursor shown) | tap at the cursor |
| D-pad up / down (cursor shown) | cursor speed |
| B | Android back |
| USB mouse | move the cursor, left click taps |

Cursor and mouse speeds are saved in `pointer.cfg`.

## Configuration

`config.txt` is created at the first launch:

| Key | Meaning |
| --- | --- |
| `joy_x` `joy_y` `joy_radius` | centre and radius of the movement joystick (fractions of the screen, 0,0 top left) |
| `look_x` `look_y` `look_speed` `invert_y` | where the look finger starts, its speed, inverted vertical look |
| `button_<A\|B\|X\|Y\|L\|R\|ZR\|PLUS\|MINUS\|RSTICK> x,y` | make a button tap a fixed point of the screen, e.g. `button_X 0.880,0.780` |
| `debug` | `1` writes a detailed `debug.log` and `watchdog.log` (for bug reports; slower) |

Saves are in `/switch/survive_nx/save/`.

## Known issues

- No in-app purchases: there is no Google Play on the Switch, so the store
  stays off and the game runs without it.
- Online Unity services (such as analytics) do not connect.

## Reporting bugs

Set `debug 1` in `config.txt`, reproduce the problem and attach `debug.log`
and `watchdog.log` from `/switch/survive_nx/`. A crash or an error screen also
writes its details into `debug.log` when `debug` is `0`.

## Building

Install devkitA64 and these portlibs:

```sh
dkp-pacman -S switch-dev switch-mesa switch-libdrm_nouveau switch-sdl2 switch-libpng switch-zlib
```

Then run `make` in a devkitPro shell. The homebrew menu shows version `0.10`;
the build number is written in the first line of `debug.log`.

`.github/workflows/build.yml` builds the `.nro` on every push in the
`devkitpro/devkita64` image and can publish a release when run by hand. It
fails when game files (`.so`, APK, `global-metadata.dat`, `dump.cs`) are
committed.

`tools/find_unity_sites.py libunity.so` finds the UnityPlayer native methods
and the Application path icalls of a `libunity.so`. `source/unity_entrypoints.h`
and `source/sv_game.c` list the other engine and game addresses and how they
were found.

## Credits

- TheOfficialFloW and Andy Nguyen, for the original Android so-loader.
- fgsfds, for the Switch so-loader groundwork.
- NaGaa95, for [subwaysurfers_nx](https://github.com/NaGaa95/subwaysurfers_nx),
  the Unity 2022.3 wrapper this port is based on.

## Support

If you like the port: [buymeacoffee.com/czn999](https://buymeacoffee.com/czn999)

## Legal

This project is not affiliated with Sandbay Games. "Survive - Wilderness
survival" is a trademark of its owner. The repository contains no game code or
data. Use files from your own legally obtained copy of the game.

## License

The source code is under the MIT license (see `LICENSE`).
