# Building and Running on Windows

## Prerequisites

- Visual Studio 2022 Community (with "Desktop development with C++" workload)
- [Wicked Engine](https://github.com/turanszkij/WickedEngine) cloned and built alongside this repo (see below)

## Building Wicked Engine (one-time setup)

Clone WickedEngine next to the bc-urnu repo so the directory structure is:

```
source/
  bc-urnu/          # this repo
  WickedEngine/     # Wicked Engine repo
```

Build it:

```
cd WickedEngine
"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" ^
  WickedEngine\WickedEngine_Windows.vcxproj ^
  -p:Configuration=Release -p:Platform=x64 -m -nologo
```

Then copy the shader compiler DLL to the bin directory:

```
copy WickedEngine\WickedEngine\dxcompiler.dll ..\bc-urnu\bin\
```

## Build

```
cd bc-urnu
"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" ^
  "src\Visual Studio solution\Bridge Command.sln" ^
  -p:Configuration=Release -p:Platform=x64 -m -nologo
```

Or open `src/Visual Studio solution/Bridge Command.sln` in Visual Studio and build Release|x64.

All executables are output to `bin/`. A post-build step copies the correct 64-bit DLLs automatically.

## Executables

| Executable | What it does |
|---|---|
| `bridgecommand.exe` | Launcher -- starts the main simulator |
| `bridgecommand-bc.exe` | Main ship simulator (Wicked Engine DX12 by default) |
| `bridgecommand-ed.exe` | Scenario editor |
| `bridgecommand-ini.exe` | Settings/INI editor GUI |
| `bridgecommand-rp.exe` | Repeater display (secondary screen) |
| `bridgecommand-mh.exe` | Multiplayer hub |
| `bridgecommand-mc.exe` | Map controller |

## Running

All executables must be run from the `bin/` directory (they look for data files relative to CWD).

```
cd bin
bridgecommand.exe
```

The launcher lets you pick a scenario, then starts the simulator with the Irrlicht renderer (full simulation, instruments, radar, etc.).

### Wicked Engine renderer (x64 only)

The x64 build includes a Wicked Engine (DX12) rendering path. When enabled, the normal scenario selection dialog appears, then the selected scenario is rendered with WE visuals (terrain, ships, buoys, land objects, ocean, sky).

Enable via command line or INI setting:

```
bridgecommand.exe --wicked
```

Or add to `bc5.ini`:

```
use_wicked_engine=1
```

The `--wicked` flag is forwarded automatically from the launcher to `bridgecommand-bc.exe`.

| Control | Action |
|---|---|
| Up/Down arrows | Port + stbd engine ahead/astern |
| Left/Right arrows | Wheel (rudder) port/starboard |
| Mouse drag | Look around (bridge mode) / orbit (orbit mode) |
| Scroll wheel | FOV (bridge mode) / zoom (orbit mode) |
| O | Toggle orbit/bridge camera mode |
| H | Horn (hold) |
| WASD | Move orbit target (orbit mode only) |
| Shift | Move faster (orbit mode) |
| Escape | Quit |

ImGui sliders for port/stbd engine, bow thruster, and wheel are also available in the HUD.

Requires `dxcompiler.dll` in `bin/` and WickedEngine shaders reachable at `../../WickedEngine/WickedEngine/shaders/` (or set `WE_SHADER_PATH`).

### Editor

```
cd bin
bridgecommand-ed.exe
```

Editor tile map controls:

| Key | Action |
|---|---|
| **T** | Toggle tile map on/off |
| **M** | Switch between satellite (ESRI) and street map (OpenStreetMap) |
| Mouse drag | Pan the map |
| Scroll wheel | Zoom in/out |

Tiles are cached to `%APPDATA%/Bridge Command/tilecache/` after first download.

## Configuration

Settings are in `bin/bc5.ini` (template) and `%APPDATA%/Bridge Command/bc5.ini` (user override). Key settings:

- `graphics_mode=2` -- windowed mode (useful for development)
- `graphics_mode=3` -- borderless fullscreen (default)
- `use_directX=0` -- OpenGL (default for Irrlicht mode), set to 1 for DirectX 9

## Debug build

Replace `-p:Configuration=Release` with `-p:Configuration=Debug` in the MSBuild command. Debug builds output PDB files alongside the executables.
