<h1 align="center">FH2Mouse</h1>

A mouse cursor for **Forza Horizon 2** on PC (Azimuth). Point at things in the menus and click them, drag the map, turn the garage camera, place decals with the keyboard, and fly the photo mode camera, all without picking up a controller. The controller keeps working as before, and you can switch between the two at any time.

FH2Mouse is a mod for the [FH2 Mod Platform](https://github.com/Trinculo54/fh2-mod-platform).

## Controls

**Everywhere in the menus**

| Mouse | Does |
|---|---|
| Move | Selects the button, tile, list row, slider or option under the cursor |
| Left click | A on what's under the cursor: opens a tile, picks a car, switches a tab |
| Left click on a slider or option | Changes it: left of the value lowers it, right of the value raises it (hold to repeat) |
| Right click | B (back) |
| Wheel | Up / down in lists, left / right in lists that scroll sideways (car lists, Jump to Manufacturer) |
| Side buttons | LB / RB |
| Middle click | Right stick click |
| Help bar at the bottom | Click an entry ("Ⓧ Return to garage", "Ⓨ Stamp", …) to press that button |

On screens with nothing to point at (the title screen, videos, the wheelspin), a left click anywhere is A.

**Map:** drag to move the map, click a spot to select the icon there or set a waypoint, wheel to zoom.

**Garage and design editor:** drag on empty space to turn the camera, wheel to zoom. In *Design editor → Edit layer*, **W A S D** move the layer (and size, spin or skew it on those tabs).

**Photo mode:** left drag to turn the camera, **W A S D** to move it, **Q / E** down / up, wheel to zoom, right click to exit.

The cursor hides by itself a few seconds after you stop moving the mouse, and comes back when you move it.

## Installing

You need Azimuth and the FH2 Mod Platform, which every platform mod ships. Extract the release zip into your Azimuth folder (the one with `azimuth.exe`), so you end up with:

```
DWrite.dll                 the FH2 Mod Platform
fh2-mods\FH2Mouse.dll      this mod
```

The zip ships the platform as `DWrite.dll` only, since Azimuth 1.0.3 no longer loads `d3dcompiler_47.dll`. If you have a `d3dcompiler_47.dll` from an older mod, it can stay; it's simply not used.

> [!IMPORTANT]
> `FH2Mouse.dll` goes in the **`fh2-mods`** folder, the FH2 Mod Platform's. Don't put it in **`mods`**: that folder belongs to Extra Options and holds its plugins, and the mod won't load from there.

Start the game and move the mouse. The first launch creates `FH2Mouse\FH2Mouse.ini` next to `azimuth.exe`.

To remove it, delete `fh2-mods\FH2Mouse.dll` and the `FH2Mouse` folder.

## Settings

`FH2Mouse\FH2Mouse.ini` (read when the game starts):

| Setting | Default | Meaning |
|---|---|---|
| `Enabled` | `1` | `0` turns the mod off |
| `HoverSelects` | `1` | Moving over something selects it. With `0`, only clicks do |
| `HideAfterSeconds` | `3` | Hide the cursor after this long without moving the mouse (`0` = never) |
| `Debug` | `0` | Log what's under the cursor and every click to `fh2-mods\platform.log` |

## Known limits

- **"Ⓨ Change time"** in the pause menu comes from the *Extra Options – Weather Control* plugin, which reads the controller directly, so clicking it does nothing. Give the plugin a keyboard key (`ChangeTimeKeyboard` in its `EO_WeatherControl.ini`) to change the time without a controller.
- The design editor's *Advanced camera* (right stick click) can't be held with the mouse.
- Only the 24 November 2015 build of the game that everyone plays is supported. On anything else the mod checks the game's code, finds it different, and stays off (it says so in `platform.log`).

## When something doesn't work

Look in `fh2-mods\platform.log` for the lines starting with `[FH2Mouse]`. If something's wrong, set `Debug=1` in the ini, reproduce it, and attach the log to an issue, together with which screen you were on.

## How it works

The game's UI library still contains a mouse system, but the Xbox build never feeds it and its hit testing finds nothing. FH2Mouse finds the control under the cursor itself, from the menus' visual tree (including the 9° perspective tilt most menus are drawn with), moves the focus there, and sends the same key presses the controller sends. Pages then behave exactly as they would with a pad. The map, the garage and photo mode cameras, and the decal editor read their inputs through a few small hooks, all made through the platform. Every hook checks the game's code first and is skipped if it isn't as expected.

## Building

FH2Mouse builds with [llvm-mingw](https://github.com/mstorsjo/llvm-mingw/releases) and CMake, with the platform as a git submodule:

```bash
git clone --recursive https://github.com/<you>/FH2Mouse
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=x86_64-w64-mingw32-clang -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-clang++ -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres
cmake --build build
```

This gives `build/FH2Mouse.dll`, and the platform as `build/platform/DWrite.dll`. Releases are built by `.github/workflows/build.yml` when a release is published (or on demand from the Actions tab), which attaches `FH2Mouse-<tag>.zip` to the release. `tools/` holds the static-analysis and test scripts used while writing it (`fh2re.py` reads the game's exe; `drive.py` moves the mouse and takes screenshots).

## License

MIT, see [LICENSE](LICENSE).

## Disclaimer

Unofficial fan-made software. Not affiliated with, endorsed by, or connected to Playground Games, Turn 10 Studios, Xbox Game Studios, Microsoft, or the Azimuth developers. Use at your own risk.
