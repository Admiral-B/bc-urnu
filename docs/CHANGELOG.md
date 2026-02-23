# Changelog: upgrade/graphics-and-simulation-overhaul

Changes relative to `main` branch (v5.10.4-alpha.4).

---

## Wicked Engine Backend (Windows x64, DX12)

Enable with `use_wicked_engine=1` in bc5.ini or `bridgecommand.exe --wicked`.

### Rendering

- PBR rendering via Wicked Engine DX12
- Terrain, own ship, other ships, buoys, land objects loaded from existing BC model/world data
- IrrlichtModelConverter converts .x/.3ds meshes to WE entities (with texture loading)
- Auto-detect PBR maps alongside textures (`wall.png` -> `wall_Normal.png`, `wall_Roughness.png`)
- OBJ importer loads TinyObjLoader PBR extensions (map_Pr roughness, map_Pm metallic)
- .3ds alpha confusion fix (format stores transparency, not opacity -- treated correctly now)
- Color-only buildings get roughness=0.55, metalness=0 for better PBR appearance
- Sky dome, sun positioned by scenario time-of-day

### Procedural Buildings (OSM)

- Queries Overpass API for building footprints at runtime for any loaded world
- Footprints extruded to 3D meshes via earcut polygon triangulation + wall extrusion
- Terrain-aware placement: each building samples heightmap at centroid
- Buildings in water (below sea level) automatically skipped
- Distance-sorted: nearest 5000 buildings to own ship rendered, capped at 200K vertices
- Split into ~50K-vertex tiles for GPU batching
- Disk cache (`buildings_cache.dat`) avoids re-querying Overpass
- Double-sided warm grey concrete material (roughness=0.65, metalness=0)
- World generation caps: 5000 buildings / 200K vertices in pre-baked buildings.obj
- No double-loading: runtime building generation skipped when pre-baked buildings.obj exists

### SimulationBridge Architecture

- Headless Irrlicht device (EDT_NULL) runs SimulationModel for physics/AI/buoys/tide/wind
- WickedMain reads state from SimBridge each frame, positions WE entities accordingly
- Zero changes to SimulationModel -- all existing physics features work immediately
- GUIMain null guards added so SimulationModel runs without GUI
- Initial engine fraction computed from `initialSpeed / maxSpeedAhead` (from boat.ini)

### ImGui HUD

- **Heading** -- Linear scrolling tape (60-degree visible range, cardinal labels, yellow lubber line)
- **Speed** -- SOG (yellow), STW (green), COG
- **Rudder** -- Arc gauge with port (red) / starboard (green), yellow needle
- **Depth** -- Numeric + vertical gauge bar, alarm threshold, trend arrow, m/ft toggle (F10)
- **Engine** -- RPM with progress bar, thrust lever percentage
- **Wind** -- Speed (knots) and direction (FROM)
- 4 OpenBridge brightness palettes (F5-F8), layout lock toggle (F9)

### Ship Controls

- **Twin screw** -- Separate port/stbd engine sliders, keyboard Up/Down moves both together
- **Bow thruster** -- Horizontal slider
- **Wheel** -- Horizontal slider (-30 to +30 degrees), keyboard Left/Right at 10 deg/s with auto-center
- Two-way binding: GUI sliders and keyboard both work; GUI drag takes priority
- Wheel command decoupled from rudder position (no feedback loop)

### Engine Sound

- Pre-decoded WAV buffer (eliminates disk I/O from audio callback)
- Loop crossfade (4096 frames) eliminates click at loop boundary
- Procedural diesel firing pulse (raised-cosine burst at 6-cylinder firing frequency)
- RPM-dependent low-pass filter (darker at idle, brighter at power)
- Constrained playback rate (0.75-1.25x) avoids unnatural pitch shift

### Camera

- Bridge first-person mode (default): camera at bridge position, follows ship heading
- Orbit mode (O key): free-orbit around target, WASD moves target
- Mouse drag for look, scroll wheel for zoom (orbit) or FOV (bridge)
- Pitch clamped to +-5 degrees, roll to +-8 degrees (prevents excessive sway)
- Camera Y heave attenuated to 30% of ship heave

---

## Physics (MMG)

Enabled for 8 ships via `MMGMode=1` in boat.ini:
ProtisSingleScrew, Protis, VIC56, VIC56_360, Puffer, HMAS_Westralia, Alkmini.

- 3-DOF hull forces (Kijima regression for coefficient estimation from hull dimensions)
- Propeller thrust via KT(J) polynomial with wake fraction and thrust deduction
- Rudder forces with propeller slipstream effect
- Added mass / hydrodynamic inertia (Clarke formulae)
- 50Hz fixed-timestep physics loop (RK2 integration)
- Shallow water effects (Gronarz power function model, Barras squat)
- Bank effects (Norrbin 1974 suction/yaw near channel walls)
- Wind forces (Isherwood 1972 model)

All other ships use legacy physics unchanged.

---

## Chart Converter & World Generation

`bc-chart-converter` (requires GDAL) converts S-57 ENC charts to BC worlds:

- Extracts buoys, lights, landmarks from S-57 layers
- Generates heightmap from DEPARE polygons + sounding interpolation (IDW)
- Merges Copernicus DEM for land elevation, BlueTopo for hi-res bathymetry
- Full world output: terrain.ini, height.png, texture.png, buoy.ini, light.ini, landobject.ini
- OSM building footprint query and mesh generation (writes buildings.obj)

Editor no-chart path (OpenSeaMap):

- Queries OpenSeaMap Overpass API for seamarks (buoys, lights, landmarks)
- Queries OSM for building footprints, generates building meshes
- Works without S-57 charts -- uses built-in coastline data + ESRI satellite tiles
- OSM island polygons (`place=island/islet`, `natural=rock/bare_rock`) rasterized as protected terrain
- Synthetic lighthouse islands (50m radius, 5m center height with quadratic falloff)
- Island protection mask prevents water subtraction, DEM flattening, and smoothing erosion
- Barrier snap radius scales with heightmap resolution (maintains ~390m physical distance)
- Comprehensive OSM queries: 43+ feature tags across buildings, water, land use, seamarks
- Terrain PBR: Sobel normal map, per-land-use roughness map, 6-octave FBM detail textures
- Building PBR: procedural wall normal maps, roughness maps, weathered roof atlas
- Default resolution 2049, elevation zoom 14, satellite zoom 19, terrain mesh limit 1024

---

## Networking

- NMEA VHW (water speed/heading), MWV (wind speed/angle) sentences
- AIS Message 5 (static/voyage data, every 6 min)
- AIS Message 21 (AtoN buoy reports, every 3 min)

---

## Audio

- OpenAL Soft backend (SoundOpenAL.hpp/cpp) with 3D spatial positioning, Doppler, HRTF
- PortAudio backend: engine sound overhauled with pre-decoded buffer + diesel synthesis
- ISound abstract interface, either backend selectable at build time

---

## Build & Test

- C++17 compiler standard
- Catch2 v3.5.2 (100 tests, 250+ assertions)
- CI test step on Linux amd64/arm64
- x64 builds link `Irrlicht_VS64.lib` (imports `Irrlicht_VS64.dll` directly)
- x86 builds link `Irrlicht.lib` (imports `Irrlicht.dll`)
- No more DLL name collision: both architectures coexist in bin/ without post-build swapping

---

## Files Added

- `src/libs/Irrlicht/irrlicht-svn/lib/Win64-visualstudio/Irrlicht_VS64.lib` -- x64 import lib (imports Irrlicht_VS64.dll)
- `src/SimulationBridge.hpp/cpp` -- Headless Irrlicht + SimulationModel wrapper for WE path
- `src/WickedMain.cpp` -- Wicked Engine application entry point and game loop
- `src/graphics/` -- Abstraction interfaces (7 headers) + Irrlicht backend (6 files)
- `src/graphics/wicked/` -- WE backend (model importer, terrain, water, ImGui, VR)
- `src/gui/ImGuiOverlay.hpp/cpp` -- ImGui instrument HUD overlay
- `src/IrrlichtModelConverter.hpp/cpp` -- .x/.3ds to WE mesh converter
- `src/PhysicsModel.hpp` -- Physics model interface
- `src/LegacyPhysicsModel.hpp/cpp` -- Extracted legacy physics
- `src/MMGPhysicsModel.hpp/cpp` -- MMG physics implementation
- `src/MMGCoefficients.hpp/cpp` -- Coefficient estimation
- `src/SoundOpenAL.hpp/cpp` -- OpenAL audio backend
- `src/ISound.hpp` -- Audio backend interface
- `src/ChartReader.hpp/cpp` -- S-57 chart reading
- `src/HeightmapGenerator.hpp/cpp` -- Heightmap from chart data
- `src/WorldGenerator.hpp/cpp` -- Full world generation pipeline
- `src/BuildingGenerator.hpp/cpp` -- Procedural building mesh from OSM footprints (earcut triangulation)
- `src/editor/OSMBuildingReader.hpp/cpp` -- Overpass API building footprint query + disk cache
- `src/libs/earcut/earcut.hpp` -- Mapbox polygon triangulation (ISC license)
- `src/chartConverter/` -- CLI chart converter tool
- `src/tests/` -- 100 Catch2 unit tests (incl. building generator)

## Files Modified (key changes)

- `src/SimulationModel.hpp/cpp` -- GUIMain null guards, getSTW(), audio/physics/AIS integration
- `src/Sound.hpp/cpp` -- Pre-decoded engine buffer, diesel synthesis, loop crossfade
- `src/OwnShip.hpp/cpp` -- MMG physics integration, abstraction types
- `src/CMakeLists.txt` -- C++17, Catch2, GDAL, OpenAL, Wicked Engine integration
- `src/AIS.hpp/cpp` -- Message 5 and 21 generators
- `src/NMEA.hpp/cpp` -- VHW, MWV sentences
- `src/FFTWave.cpp` -- JONSWAP spectrum
- `src/graphics/wicked/WickedModelImporter.cpp` -- PBR auto-detection (normal/roughness maps)
- `src/editor/EditorApp.hpp/cpp` -- Building footprint overlay (B key), OSM building generation in no-chart path
- 40+ headers -- irrlicht.h removed, bc::graphics types adopted
- 8 boat.ini files -- MMGMode=1 added
