# Bridge Command Upgrade Plan

**Branch:** `upgrade/graphics-and-simulation-overhaul`
**Updated:** February 2026

## Constraints

- 2 networked PCs, 6 monitors (3 bridge windows + radar/helm/charts). Never break this.
- ENet protocol must remain byte-compatible.
- All 13 scenarios and 6 worlds must load. All 17 ownships and 41 AI ships must work.
- Windows primary, macOS/Linux secondary.

## Architecture Decisions

| Decision | Choice | Rationale |
|---|---|---|
| Graphics engine | **Wicked Engine** (MIT, DX12/Vulkan/Metal) | Best ocean rendering, active development, C++17, ECS |
| Physics model | **MMG 3-DOF** (Yasukawa & Yoshimura) | Industry standard, Kijima coefficient estimation from hull dims |
| Audio | **PortAudio** (existing) with pre-decoded engine buffer + procedural diesel synthesis | OpenAL Soft coded but WE path uses existing PortAudio |
| GUI | **Dear ImGui** over WE render path | Immediate-mode, MIT license, OpenBridge color palettes |
| WE integration | **SimulationBridge** pattern -- headless Irrlicht device for physics, WE for rendering | Zero changes to SimulationModel physics code |
| Chart import | **GDAL/OGR** for S-57 ENC parsing | MIT license, built-in S-57 driver |

## What's Implemented

### Wicked Engine Backend (`WickedMain.cpp`)

- PBR rendering with DX12 (terrain, ships, buoys, land objects, procedural buildings, sky)
- SimulationBridge wraps full SimulationModel (MMG physics, AI ships, buoys, tide, wind)
- ImGui HUD: linear heading tape, SOG/STW/COG, rudder arc, depth gauge, engine RPM, wind
- Twin screw controls (port/stbd engine sliders + bow thruster + wheel at +-30 deg, 10 deg/s)
- Bridge first-person camera with mouse look, orbit mode toggle (O key), pitch/roll clamping
- Engine sound: pre-decoded WAV buffer with procedural diesel synthesis, RPM-dependent filtering
- Per-frame sync of other ship positions and buoy positions from SimulationModel
- .3ds alpha confusion fix (transparency vs opacity)
- Color-only building materials improved (roughness/metalness tuning)
- PBR auto-detection: `_Normal.png` / `_Roughness.png` alongside base textures
- Initial engine fraction from `initialSpeed / maxSpeedAhead` (boat.ini)

### Procedural Buildings (OSM)

- `OSMBuildingReader` queries Overpass API for building footprints in world bbox
- `BuildingGenerator` extrudes footprints to 3D meshes (earcut triangulation + wall quads)
- Terrain-aware Y placement, water culling, distance-sorted (nearest 5000, 200K vert cap)
- Split into ~50K-vertex tile batches for GPU efficiency
- Disk cache (`buildings_cache.dat`) avoids repeated API queries
- Wired into both WE runtime (any scenario) and editor world generation (no-chart path)
- World generation capped at 5000 buildings / 200K vertices (prevents GPU overload)
- Runtime skips building generation when pre-baked buildings.obj exists (no double-loading)

### Physics (MMG)
- 3-DOF hull forces, propeller KT(J), rudder with slipstream
- 50Hz fixed-timestep RK2 integration
- Shallow water (Gronarz), bank effects (Norrbin), wind forces (Isherwood 1972)
- 8 ships enabled: ProtisSingleScrew, Protis, VIC56, VIC56_360, Puffer, HMAS_Westralia, Alkmini
- Legacy physics unchanged for all other ships

### Chart Integration & World Generation

- S-57 reader (buoys, lights, coastlines, depths, landmarks)
- Heightmap generator (DEPARE + sounding IDW + Copernicus DEM merge)
- World generator pipeline (terrain.ini + height.png + texture.png + buoy/light/landobject.ini)
- OSM building query + mesh generation integrated into both chart and no-chart paths
- Editor no-chart path: OpenSeaMap seamarks + OSM buildings + coastline data + ESRI satellite

### Networking
- NMEA VHW, MWV sentences
- AIS Message 5 (static/voyage), Message 21 (AtoN buoys)

### Build & Test

- C++17, Catch2 (100 tests, 250+ assertions), CI on Linux amd64/arm64
- x64 builds link `Irrlicht_VS64.lib` / x86 link `Irrlicht.lib` (no DLL name collision in bin/)

## Remaining Work

### High Priority
- Performance testing on bridge hardware (6-monitor setup)
- Multi-window WE rendering for 3 bridge monitors
- WE ocean rendering (GPU FFT, multi-cascade, foam) -- WE has built-in ocean, needs tuning
- Irrlicht path regression testing (verify nothing broken)

### Medium Priority
- Wave-ship interaction forces (sample wave field at hull points for roll/pitch)
- JONSWAP spectrum in Irrlicht FFTWave.cpp (currently Phillips)
- AIS Message 18 (Class B for pleasure craft)
- VR rendering through WE (OpenXR integration)
- NMEA VDR (set & drift), MTW (water temperature)

### Low Priority / Future
- 6-DOF physics extension (heave/roll/pitch from wave excitation)
- Ship-ship interaction forces
- GPU-accelerated radar rendering
- ECDIS display integration
- Dual S-band/X-band radar
- Volumetric fog

## Key References

- Yasukawa & Yoshimura (2015), "Introduction of MMG Standard Method"
- Fossen, "Handbook of Marine Craft Hydrodynamics and Motion Control"
- Wicked Engine: https://github.com/turanszkij/WickedEngine
- MSS Toolbox: https://github.com/cybergalactic/MSS
- OpenBridge design system: https://www.openbridge.no/
