# Ketchup Plan: Phases 4-12

## TODO

### Phase 13: Standard Controls Completeness

- [x] Burst 51: Ship capability queries -- added isSingleEngine(), hasBowThruster(), hasSternThruster() to SimBridge. HUD adapts: single-engine ships get one centered slider, thrusters only shown when present. [depends: none]
- [x] Burst 52: Stern thruster support -- wired setSternThruster() through WickedMain game loop + ImGui overlay. Keyboard: Z/X=bow thruster port/stbd, C/V=stern thruster port/stbd. Auto-return to zero. [depends: 51]

### Phase 12: In-Game Tuning and Polish

- [ ] Burst 46: Wave motion tuning -- verify motion feel across ship types, weather levels, heading-to-sea angles. Pre-work done: added GM=0.6/RollPeriod=7/PitchPeriod=5 to 3111_Tug, GM=0.7 to Aquarius_Tug. Remaining 11 ownships use defaults (B*0.06 GM, 8s roll, 12s pitch). Needs in-sim testing. [depends: 45]
- [ ] Burst 33: GLB ship tuning -- verify bridge camera positions, model waterline alignment, physics feel for all 4 GLB ships as ownship. Pre-review done: configs look correct (view coords are model-space pre-scale, Perry's large values are expected). Needs in-sim testing. [depends: 32]
- [x] Burst 34: Ownship-specific model enhancements -- MakeTransparent already works in WE path (alpha detection). radar.ini added for 9 missing ships (5 ownships + 4 GLB otherships). BridgeHalfWidth/BridgeHalfDepth boat.ini overrides for walk bounds. [depends: 33]

### Phase 11: Azimuth Drive Support in WickedMain (DEPRIORITISED)

WickedMain.cpp has azimuth keyboard controls (Bursts 47-49). ImGui schottel dial widgets scaffolded but not wired into game loop.

- [x] Burst 47: SimBridge azimuth API -- added isAzimuthDrive(), btn*Schottel/ThrustLever(), get*Schottel/AzimuthThrustLever() to SimulationBridge.hpp/cpp. All delegate to existing SimulationModel methods. [depends: none]
- [x] Burst 48: WickedMain azimuth keyboard -- detect isAzimuthDrive at ship load. Arrow Up/Down=both thrust levers, Arrow Left/Right=both schottels. WASD reserved for bridge walk. Individual drive control deferred to mouse/joystick UI. [depends: 47]
- [x] Burst 49: WickedMain azimuth HUD -- engine display shows "Azimuth Drives" with port/stbd schottel angles and thrust lever %. SimulationHUDData extended with azimuth fields. [depends: 48]
- [ ] Burst 50: Azimuth mouse/joystick UI -- ImGui schottel dial widgets coded (renderAzimuthControls), need wiring into game loop readback. Deferred. [depends: 49]

### Phase 15: WickedEngine 3D Gaussian Splatting Integration

WE added 3DGS support March 2-4 2026 (PRs #1571-#1575). WE lib updated to include it. Enables photorealistic scanned environments and objects via .PLY Gaussian splat files. glTF/GLB can also embed 3DGS data (Khronos added to spec Aug 2025).

**Potential BC uses:**
- Scanned harbours/marinas as environment assets (replace satellite texture + heightmap)
- High-fidelity landmark models (lighthouses, port infrastructure)
- Visual-only othership models from real vessel scans
- Training scenario backgrounds from real-world locations

**Status:**
- [x] Burst 58: WE lib updated to latest master with 3DGS (wiGaussianSplatModel.cpp/h, gaussian_splat shaders, PLY importer). Merge conflicts resolved in oceanSurfacePS/VS.hlsl and wiOcean.h. [depends: none]
- [x] Burst 59: 3DGS PLY loading in BC scene -- ported miniply.h/cpp and ImportModel_PLY from WE Editor. LoadModelFromFile handles .ply (mesh and 3DGS). splats.ini world loader (File/Long/Lat/Height/Rotation/Scalefactor/Absolute per entry). Tested in-sim: Voxel51 train_7k.ply (741k splats) renders correctly with ocean/terrain. Height fix: Absolute=1 for sea-level placement, Absolute=0 clamps terrain to max(0, terrainH). [depends: 58]
- [ ] Burst 60: 3DGS automated pipeline integration -- see Phase 16 below. [depends: 59]

### Phase 16: Automated 3DGS in World Generation Pipeline

**Goal:** Integrate Gaussian splat assets into the world generation pipeline so that generated worlds automatically include photorealistic scanned objects alongside procedural geometry.

**Architecture:** The world generator currently outputs terrain (heightmap + satellite texture), navigation aids (buoys, lights), landmarks (landobject.ini), and buildings (OSM extrusion). 3DGS adds a new asset layer: pre-scanned real-world objects placed at geographic coordinates.

**Two integration paths:**

**Path A: Curated splat library (near-term)**
Maintain a shared library of scanned assets (`Models/Splats/`) indexed by type and geographic region. WorldGenerator writes `splats.ini` when it detects landmarks or port infrastructure that have matching splat assets.

- [ ] Burst 61: Splat asset manifest -- `Models/Splats/manifest.ini` indexes available .ply files by category (lighthouse, harbour_wall, pier, crane, lock_gate, buoy_cluster) and geographic region. Each entry has bounding box dimensions for correct scale placement. [depends: 59]
- [ ] Burst 62: WorldGenerator splat matching -- during landobject.ini generation, check manifest for splat assets matching landmark types (e.g., S-57 CATLMK=lighthouse -> splat library lookup). Write matching entries to splats.ini with correct Long/Lat/Height from chart data. Fallback to procedural model if no splat available. [depends: 61]
- [ ] Burst 63: Splat LOD/culling -- large splat files (100MB+) need distance-based loading. Add MaxDistance field to splats.ini. WickedMain skips LoadModelFromFile for splats beyond threshold, loads on approach. Prevents memory exhaustion with many splats. [depends: 62]

**Path B: On-demand capture pipeline (future)**
Generate 3DGS from street-level imagery APIs or drone footage. Requires GPU compute.

- [ ] Burst 64: Street-level imagery integration -- query Mapillary/Google Street View API for geotagged images near key landmarks during world generation. Download image sets covering 30+ angles per subject. [depends: 63]
- [ ] Burst 65: Automated splat training -- invoke OpenSplat/Nerfstudio CLI from WorldGenerator to train 3DGS from downloaded images. Output .ply to world's splat directory. GPU-intensive (~5-15 min per object on RTX 3060+). [depends: 64]
- [ ] Burst 66: Splat post-processing -- auto-crop trained splats (remove sky/ground floaters), estimate bounding box, compress via SuperSplat CLI, write manifest entry. [depends: 65]

**Path C: Community splat sharing (future)**
- [ ] Burst 67: Splat upload/download service -- share scanned harbour assets between BC users. Server indexes by lat/lon bounding box. WorldGenerator queries service during generation and downloads matching splats. Similar to existing tile cache but for 3DGS assets.

**Priority:** Path A is implementable now (just needs curated .ply files). Path B requires significant compute and API access. Path C requires server infrastructure.

### Phase 17: Terrain and Building Photorealism

**Goal:** Improve close-range visual quality of terrain and buildings using existing pipeline infrastructure. No scans needed.

- [x] Burst 68: Generate missing PBR maps for shipped worlds -- tools/generate_pbr_maps.py reads height.f32 or height.png, generates Sobel normal.png + BFS roughness.png. Supports both float32 and uint8 heightmaps. Generated for all 6 worlds. [depends: none]
- [x] Burst 69: Building facade textures -- tools/generate_building_textures.py generates 1024x1024 wall atlas (brick/concrete/stone/stucco) + 1024x512 roof atlas (slate/terracotta/brown/zinc) with normal and roughness maps. Matches EditorApp.cpp procedural generation. Generated for PortsmouthHarbour, SantaCatalina, SwinomishChannelSouth. [depends: none]
- [x] Burst 70: Tiling terrain detail textures -- tools/generate_terrain_detail.py generates 256x256 tileable noise. WickedTerrainNode loads via OCCLUSIONMAP with UV set 1 tiled at ~4m intervals. Adds micro-surface variation to satellite texture. [depends: none]
- [x] Burst 71: Billboard trees from OSM forest -- tools/generate_trees.py scatters trees on land (height>2m, eroded from water edge) with noise-modulated density. X-shaped cross-billboards batched into single mesh. tree_billboard.png procedural alpha-tested sprite. trees.ini format with lon/lat/height/scale/rotation. [depends: none]
- [x] Burst 72: OSM road geometry -- tools/generate_roads.py queries Overpass for highway=* ways, extrudes flat ribbon geometry at terrain height (+15cm). roads.obj + roads.mtl with dark asphalt material. WickedMain loads via OBJ importer with road material override. [depends: none]

### Phase 14: Photorealistic Roadmap Items

- [x] Burst 53: Lightning storm effect -- point light in cloud layer at B8+. Random strike position 200-1000m from camera, 0.2-0.4s flash with forked re-flash decay. Ambient boost during flash. Frequency increases with Beaufort. [depends: none]
- [x] Burst 54: Nav light flare improvement -- 8x8 radial Gaussian textures replace 4x4 flat squares. Natural point-light glow appearance. [depends: none]
- [x] Burst 55: Post-processing pipeline upgrade -- TAA (temporal AA), chromatic aberration (0.5), sharpen filter (0.15 to counteract TAA softening), dithering (reduces sky banding). [depends: none]
- [x] Burst 56: Enhanced time-of-day transitions -- golden hour (deep warm sun color at low angles), blue hour (cool blue ambient 30min after sunset/before sunrise), moonlight ambient at night, dynamic sky exposure. Water color darkens at night. [depends: none]
- [x] Burst 57: Buoy wave bobbing -- buoys now sample ocean wave height at their position, bob realistically on waves instead of floating at fixed height. [depends: none]
- [ ] Burst 40: Whitecap foam -- DEFERRED. Jacobian fold values too small at choppy_scale*0.3 regime. Needs either engine-level choppy_scale decoupling or alternative foam approach. [depends: 39]

### Phase 9: Ocean Shader Realism

- [x] Burst 35: WE spectrumCallback + randomSeed extension -- added to OceanParameters (wiOcean.h), initHeightMap() (wiOcean.cpp). Rebuilt WE lib. [depends: none]
- [x] Burst 36: OceanCB layout extension -- added normalOverlayIndex, cascade0/2 grad indices and weights to ShaderInterop_Ocean.h + OceanParameters. Deleted all .cso/.wishadermeta. Rebuilt WE lib. [depends: 35]
- [x] Burst 37: Normal map overlay -- 512x512 FBM noise normal texture, bindless in oceanSurfacePS.hlsl, 500m period with slow animation [depends: 36]
- [x] Burst 38: Multi-cascade blending -- REVERTED. Aux cascades share 50m patch_length, creating checkerboard artifacts. JONSWAP reverted: peak at typical winds outside FFT range. Phillips retained. [depends: 37]
- [x] Burst 39: patch_length 50->250m -- eliminates geometric tiling at root cause. K_min drops from 0.126 to 0.025 rad/m, Phillips K^-6 gives ~15000x more energy per mode. BEAUFORT_AMPLITUDE reduced from {2..1700} to {2..50}. choppy_scale 3x compensates GridLen reduction. Normal Y scale hardcoded to 0.2 (decoupled from xOceanTexelLength). Multi-scale VS/PS displacement hacks removed (single clean FFT sample). [depends: 38]

## DONE

### Phase 10: Wave-Coupled Ship Motion

- [x] Burst 41: WaveMotionModel.hpp -- second-order damped oscillators for heave/pitch/roll, wavelength reduction (sinc filter), added resistance in waves (Stawave-1 ITTC), rudder sea state factor. Header-only, pure math. [depends: 39]
- [x] Burst 42: OwnShip wave coupling -- replaced fake sinusoidal pitch/roll with wave-surface-driven oscillators. 5-point wave sampling (CG, bow, stern, port, stbd). Added RAW to both MMG and legacy drag. Directional yaw buffeting (beam seas > head seas). Rudder degradation in rough weather. [depends: 41]
- [x] Burst 43: OtherShip wave coupling -- same oscillator model, initialized from bounding box dimensions. OtherShips wrapper now passes raw tideHeight (heave handled internally). Pitch/roll applied to rotation. [depends: 42]
- [x] Burst 44: Catch2 tests (23 cases, 52 assertions) -- oscillator convergence, resonance amplification, wavelength reduction, RAW formula, rudder degradation, ship-type differentiation. [depends: 41]
- [x] Burst 45: boat.ini GM + periods for key ships -- CargoShip, HMS_Clyde, USS_Perry, USS_Zumwalt, Waverley, Atlantic85. [depends: 42]

### Phase 8: GLB Ship Model Integration

- [x] Burst 29: `tools/glb_inspect.py` parses GLB, extracts geometry bounds and PBR texture inventory [depends: none]
- [x] Burst 30: Script generates starter boat.ini with estimated scale, camera views, physics, nav lights [depends: 29]
- [x] Burst 31: Four GLB othership models added -- CargoShip (170m Handysize), HMS_Clyde (81.5m OPV), USS_Perry (FFG-7 135.6m), USS_Zumwalt (DDG-1000 186m) with hand-tuned boat.ini, nav lights, angle corrections [depends: 30]
- [x] Burst 32: Ownship directory fallback -- WickedMain.cpp ownship loader now checks Models/Othership/ when model not found in Models/Ownship/ (mirrors existing othership fallback). boat.ini files updated with Views (bridge/wing/overhead), maxSpeedAhead, basic physics for all 4 GLB ships [depends: 31]

### Phase 7: Photorealism Improvements

- [x] A1: Post-processing -- HBAO (range=2, power=2), eye adaptation (key=0.08), light shafts (0.03), exposure (1.1)
- [x] A2: Building wall PBR at runtime -- load normal/roughness maps in createBuildingMeshEntity
- [x] A3: Building roof PBR at runtime -- same pattern for roof material
- [x] B1: Roof normal + roughness map generation in editor (tile-edge Sobel + per-material roughness)
- [x] B2: Wall texture quality -- ground-floor dirt, per-type color temperature, rain streaks
- [x] B3: Satellite zoom +1 -- REVERTED (1092 tiles exceeded 2min timeout, was 300 at zoom 15)
- [x] C1: Terrain normal map strength increased to 1.8

### Phase 4-6: Terrain Textures, Tile Performance, GBA Heights

- [x] Bursts 1-3: Sobel normal map from heightmap (TerrainNormalMap)
- [x] Bursts 4-5: Roughness classification per land-use type (TerrainRoughnessMap)
- [x] Bursts 6-10: Slope/water/elevation detail texture blending (DetailTextureBlending)
- [x] Bursts 11-13: Thread pool with rate limiting (TileThreadPool)
- [x] Bursts 14-16: WinHTTP connection pooling (WinHTTPConnectionPool)
- [x] Bursts 17-18: Multi-threaded tile downloader (TileDownloaderMultiThread)
- [x] Bursts 19-20: Two-pass progressive world generation (ProgressiveWorldGen)
- [x] Bursts 21-24: GBA tile download + GeoJSON parser (GBATileDownloader)
- [x] Bursts 25-28: GBA height enrichment with provenance tracking (GBAHeightEnrichment)
