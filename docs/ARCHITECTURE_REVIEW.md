# Bridge Command Architecture Review & Task Breakdown

**Date:** February 2026
**Branch:** `upgrade/graphics-and-simulation-overhaul`
**Goal:** Multi-monitor bridge simulator with networked instrument stations, photorealistic visuals, chart-based world generation, and eventual online multiplayer.

---

## Table of Contents

1. [Current Architecture Assessment](#1-current-architecture-assessment)
2. [Target Setup vs Current Capabilities](#2-target-setup-vs-current-capabilities)
3. [Industry Context](#3-industry-context)
4. [Critical Architecture Issues](#4-critical-architecture-issues)
5. [Development Approach: TDD](#5-development-approach-tdd)
6. [Task Breakdown](#6-task-breakdown)

---

## 1. Current Architecture Assessment

### What Exists Today

| Component | Technology | State |
| --- | --- | --- |
| 3D rendering (legacy) | Irrlicht (OpenGL) | Working, dated visuals |
| 3D rendering (new) | WickedEngine (DX12/Vulkan) | Partially working -- visual-only, missing radar/NMEA/networking |
| GUI - simulation | Irrlicht built-in GUI | Working, looks circa 2005 |
| GUI - editor | ImGui (Win32+OpenGL) | Working, modern |
| GUI - multiplayer hub | ImGui (Win32+OpenGL) | Working, modern |
| Physics - legacy | Empirical drag/thrust model | Working |
| Physics - MMG | Yasukawa/Yoshimura hydrodynamics | Working, well-abstracted |
| Networking | ENet (UDP) + custom text protocol | Working, fragile |
| NMEA output | NMEA 0183 over UDP/serial | Working (Irrlicht path only) |
| Radar | 3D scene-based radar rendering | Working (Irrlicht path only) |
| Multi-monitor | Win32 EnumDisplayMonitors + manual INI | Working, no setup wizard |
| Multi-computer | Primary/Secondary modes via UDP | Working |
| Chart reading | GDAL S-57 (x86 only) | Working |
| World generation | ChartReader + HeightmapGenerator + SatelliteTexture | Working |
| Sound | PortAudio + optional OpenAL (3D audio) | Working |
| VR | OpenXR | Working (Irrlicht path only) |
| Tests | Catch2 via CMake | ~15 test files covering core subsystems |

### Architecture Strengths

- **Physics abstraction** (`IPhysicsModel` interface): Clean, extensible. MMG model implements real hydrodynamics (Kijima coefficients, Barras squat, Norrbin bank forces, Isherwood wind). New models can be added without touching existing code.
- **Sound abstraction** (`ISound` interface): Two backends (PortAudio, OpenAL) behind a clean interface.
- **Network factory** (`Network::createNetwork()`): Correct implementation per operating mode (Normal, Secondary, Multiplayer, MultiplayerClient).
- **Graphics abstraction layer** (`bc::graphics` namespace): `IRenderer`, `ISceneManager`, Irrlicht-free types (`Vec2`, `Vec3`, `Quaternion`). Foundation exists but is not yet used by the main simulation path.
- **SimulationBridge**: Pure C++ facade over SimulationModel with zero Irrlicht types. Allows WickedEngine to drive simulation without Irrlicht headers.
- **World generation pipeline**: Well-structured with single-responsibility components. S-57 chart reading, IDW heightmap interpolation, satellite tile compositing, buoy/light/landmark extraction all work.
- **Test infrastructure**: Catch2 tests cover physics, chart reading, heightmap generation, world generation, multiplayer coordination. Tests build against `bc-testable` static library, proving domain logic separability.

### Architecture Weaknesses

- **Monolithic main()**: 1,452 lines. All bootstrap, config, window creation, device init, scenario selection, network setup, GUI, and game loop in one function.
- **SimulationModel god object**: 454-line header, 2,208-line implementation. Simultaneously manages physics, scene, radar, cameras, GUI, sound, mooring lines, weather, and time. 150+ public methods.
- **Irrlicht types in core domain**: `SimulationModel`, `OwnShip`, `Ship`, `Terrain`, `GUIMain` all store raw `irr::*` types. This prevents reuse in the WickedEngine path.
- **Duplicated simulation loops**: `main.cpp` (Irrlicht path) and `WickedMain.cpp` (2,028 lines) are parallel implementations. Every feature/bugfix must be done twice. The WickedEngine path is missing radar, NMEA, VR, networking, joystick, and mooring lines.
- **Unversioned network protocol**: Hand-built delimited strings with positional parsing. Adding a field breaks all existing clients. Fixed 8KB buffer with FIXME comment.
- **OwnShip overloaded**: 2,909 lines, 70+ member variables. Navigation, two physics models, azimuth drives, rudder pump simulation, thrusters, collision detection, and visual display all in one class.
- **No configuration validation**: 100+ INI values read inline with silent 0 defaults on missing keys.
- **Editor is Windows-only**: Raw Win32 + WGL. No SDL2/GLFW abstraction.

---

## 2. Target Setup vs Current Capabilities

### Your Target Configuration

```
PC 1 (Bridge Visual Station):
  TV 1: Port bridge window    (bridgecommand-bc, look_angle=-90)
  TV 2: Center bridge window  (bridgecommand-bc, look_angle=0)
  TV 3: Starboard bridge window (bridgecommand-bc, look_angle=90)

PC 2 (Instrument Station):
  Monitor 1: Helm + rudder repeater (bridgecommand-rp / custom)
  Monitor 2: Radar display (bridgecommand-bc in full-radar mode, or standalone)
  Monitor 3: Chart viewer (OpenCPN via NMEA, or built-in map controller)
```

### Current Support Assessment

| Requirement | Supported? | Gap |
| --- | --- | --- |
| 3 TVs as bridge windows on one PC | Partial | Must launch 3 separate instances, manually edit INI for each. No setup wizard. Irrlicht path works; WickedEngine path has no multi-view networking yet. |
| Networked instrument station | Yes | Secondary mode + repeater/controller apps exist. UDP protocol works. |
| Helm controls on instrument PC | Partial | `bridgecommand-rp` shows heading/rudder repeaters. Engine controls require a secondary `bridgecommand-bc` instance. Controls are abstract sliders, not realistic gauges. |
| Radar on instrument PC | Partial | Can run `bridgecommand-bc` in `full_radar=1` mode on instrument PC. Radar calculation is sophisticated (ARPA, sea/rain clutter, multiple ranges). But radar is Irrlicht-only. |
| Chart viewer (OpenCPN) | Yes | NMEA 0183 output over UDP port 10110 already exists. OpenCPN connects directly. Documented and tested. |
| Built-in chart viewer | Yes | Map Controller (`bridgecommand-mc`) exists as instructor station. |
| Photorealistic visuals | No | Irrlicht is dated OpenGL. WickedEngine has FFT ocean, volumetric clouds, atmospheric scattering, ray tracing -- but its integration is incomplete. |
| World generation from charts | Partial | S-57 pipeline works (NOAA charts tested). Produces terrain, heightmap, texture, buoys, lights. Missing: building generation from OSM, detailed terrain textures, vegetation. |
| Realistic ship handling | Yes | MMG physics model is production-quality. Supports shallow water, bank effects, wind, azimuth drives. |
| Online multiplayer | Partial | Multiplayer hub exists with ENet. Supports peer join/leave, ship assignment, chat. Protocol is fragile and unversioned. No authentication, no NAT traversal. |

### Verdict

The fundamental architecture for your target setup **exists and works** in the Irrlicht path. The critical gap is completing the WickedEngine integration so you get modern visuals while retaining all the networking, radar, NMEA, and instrument functionality. Secondary gaps are UX polish (setup wizard, realistic instruments) and protocol robustness for online multiplayer.

---

## 3. Industry Context

### Professional Simulator Standards (DNV-ST-0033)

| Class | Description | Display | Bridge Command Equivalent |
| --- | --- | --- | --- |
| Class A | Full mission | 240-270 deg FOV, 7-12 displays, physical instruments | Target: 3-TV setup approximates lower-end Class A |
| Class B | Part task | Reduced environment, specific training objectives | Current multi-screen setup |
| Class C | Desktop | Standard computer workstation | Current single-screen mode |
| Class D | Online/eLearning | Web or cloud-based | Stretch goal (online multiplayer) |

The 2025 DNV-ST-0033 revision (effective Jan 2026) opens certification to VR and cloud-based simulators. Bridge Command's OpenXR support is forward-looking.

### Commercial Competitor Landscape

| Vendor | Rendering | Ocean | Price |
| --- | --- | --- | --- |
| Wartsila (ex-Transas) | Proprietary | Professional | $100K-$5M |
| Kongsberg K-Sim | Proprietary | Professional | $100K-$5M |
| VSTEP NAUTIS | UNIGINE 2 Sim | Beaufort-accurate | $50K-$2M |
| **Bridge Command** | **Irrlicht / WickedEngine** | **FFT ocean (WE)** | **Free (GPLv2)** |

Bridge Command is the only actively-maintained open-source full-featured bridge simulator. No direct open-source competitor exists.

### WickedEngine Assessment for Maritime Use

**Suitable.** MIT license, manageable C++ codebase, native DX12+Vulkan. Key features:
- FFT ocean simulation (infinite, camera-relative)
- Volumetric raymarched clouds with weather map
- Rayleigh atmospheric scattering
- Planar reflections for water
- Wetness system (objects entering water)
- Ray tracing (DX12 + Vulkan)
- Bindless rendering pipeline

**Gaps needing custom development:** Beaufort-scale wave parameterization, ship wake simulation, maritime fog/visibility, shoreline wave interaction, multi-channel output for multi-display setups.

### Free Nautical Chart Sources

| Source | Format | Coverage |
| --- | --- | --- |
| NOAA | S-57 ENC | US coastal + inland (best free source) |
| LINZ | S-57 ENC | New Zealand |
| Brazil DHN/CHM | S-57 ENC | Brazil coast |
| Kartverket | Raster | Norway |
| OpenSeaMap | OSM overlay | Global seamarks |
| EMODnet | Bathymetry grid | European waters |
| GEBCO | Bathymetry grid | Global (4GB+) |

UK and most European ENCs are commercial (UKHO/IC-ENC/PRIMAR). For non-US/NZ waters, world generation relies on OSM coastlines + GEBCO bathymetry + satellite imagery -- which is what the current pipeline already does.

---

## 4. Critical Architecture Issues

These must be addressed roughly in this order. Each feeds into the next.

### Issue 1: Simulation and Rendering Are Entangled

The simulation (physics, networking, NMEA, radar, weather) is inseparable from Irrlicht rendering types. This forces duplication of the entire simulation loop for WickedEngine and means features only work in one renderer.

**Impact:** Every feature works in Irrlicht OR WickedEngine, never both. Maintenance cost doubles.

### Issue 2: WickedEngine Path Is Feature-Incomplete

`WickedMain.cpp` (2,028 lines) is a standalone reimplementation missing: radar, NMEA output, VR, joystick input, multi-computer networking, mooring lines, sound integration, HUD instruments, and secondary display support.

**Impact:** Cannot use photorealistic visuals for the target bridge setup.

### Issue 3: No Multi-View Within WickedEngine

The Irrlicht path supports multi-view via separate process instances connected over UDP. WickedEngine has `WickedMultiView` (multi-window swap chains) but it is not connected to the networking or simulation. Running 3 WickedEngine instances on one PC requires 3x the GPU memory with no frame synchronization.

**Impact:** Three TVs from one PC will have inconsistent frame timing and tripled resource usage.

### Issue 4: Network Protocol Is Fragile

Hand-built delimited strings with positional field parsing, no version negotiation, fixed 8KB buffer. Adding any field breaks all clients.

**Impact:** Cannot evolve the protocol for online multiplayer without breaking existing setups.

### Issue 5: UX for Multi-Station Setup Is Manual

Setting up the target configuration requires editing INI files across multiple instances, understanding UDP networking, and launching processes manually. No setup wizard, no visual configuration.

**Impact:** Only technically proficient users can achieve the multi-monitor setup.

### Issue 6: World Generation Fails on Barrages and Enclosed Harbours

**Status: Fixed (2026-02-20). Both code paths now handle barriers correctly.**

There are **two completely separate world generation code paths** in the editor, and both had barrier-handling problems:

**Path A: S-57 chart loaded** (`EditorApp.cpp:2130-2178` -> `WorldGenerator::generateWorld()`)
Uses `HeightmapGenerator` with GDAL. S-57 chart features (buoys, lights, coastlines, depth areas) are extracted and used to build the heightmap. Barrier flood-fill was **missing** from this path until 2025-02-19.

**Path B: No chart loaded** (`EditorApp.cpp:2181-2703`, inline code)
Uses Natural Earth coastlines + OSM Overpass queries. Barrier data extracted from the building query (`OSMBuildingReader`). This path is taken for any area without free S-57 charts (UK, most of Europe).

**Root cause trace (Cardiff Bay example):**

1. No free UK S-57 chart exists -- generation takes **Path B** (no-chart fallback)
2. ~~Path B queries Overpass API separately for `waterway=dam` and `man_made=breakwater`~~ Fixed 2026-02-20: barrier data now extracted from the building query, eliminating the separate (often rate-limited) Overpass request
3. Cardiff Bay Barrage is correctly tagged in OSM (way 1287700755, `waterway=dam`, 62-node closed polygon)
4. ~~Overpass API frequently returns 504 timeout for barrier query~~ Fixed: barriers extracted from building query which already succeeds
5. ~~Cardiff Bay Barrage is a closed polygon (first == last point). Original endpoint snapping only checked first/last vertices (identical), so only one end got connected to land. BFS flood entered around the unsnapped end~~ Fixed 2026-02-20: vertex snapping now checks ALL vertices
6. ~~8-pass 5x5 box blur ran AFTER barrier flood-fill, eroding thin barrier lines (1-3px) back below sea level~~ Fixed 2026-02-20: blur now runs BEFORE flood-fill (task 0.3c)

**All barrier bugs now fixed:**

- ~~Uses `< 0.0f` instead of `<= 0.0f` in flood-fill comparisons -- IEEE 754 `-0.0f` bug~~ Fixed 2025-02-19 (task 0.3b)
- ~~No error feedback to user when Overpass times out~~ Fixed 2025-02-19 (task 0.3b)
- ~~Inline flood-fill is a full copy of the `HeightmapGenerator` algorithm with no code reuse~~ Fixed 2026-02-19 (task 0.3a): extracted `BarrierFloodFill` standalone utility
- ~~Separate Overpass barrier query rate-limited alongside building + OpenSeaMap queries~~ Fixed 2026-02-20: barrier data extracted from building query
- ~~Closed polygon vertex snapping only checked first/last (identical) endpoints~~ Fixed 2026-02-20: checks all vertices
- ~~Smoothing blur erodes barrier heights back below sea level~~ Fixed 2026-02-20 (task 0.3c): blur runs before flood-fill
- ~~`OSMBuildingReader.query()` does not clear `barrierLines` on re-query~~ Fixed 2026-02-20 (task 0.3d)
- ~~`OSMBuildingReader.saveCache()` does not persist `barrierLines`~~ Fixed 2026-02-20 (task 0.3d)

**S-57 chart path (originally reported, now fixed):**

The original root cause was that `isLand()` in `HeightmapGenerator.cpp` only tests closed polygons (`closeDist < 0.0001`), silently discarding open linestrings like barrages. Fixed by adding `setBarriers()` + `applyBarrierFloodFill()` to `HeightmapGenerator` and wiring it through `WorldGenerator`.

**Remaining compounding factors:**

- OpenSeaMap is only used as a fallback when no S-57 chart is loaded, never to supplement chart data
- Lighthouse nodes from OpenSeaMap are imported as landmarks but their `seamark:light:*` properties are discarded
- Chart-based generation produces colour-coded elevation textures, not satellite imagery
- ~~**Code duplication**: Path B has ~300 lines of inline heightmap generation that duplicates `HeightmapGenerator` functionality~~ Fixed: barrier flood-fill extracted to shared `BarrierFloodFill` utility (remaining duplication: ~150 lines of smoothing and RGB encoding)

**Known thread safety issues (latent, not yet fixed):**

- `generateStatus` (`std::string`) is written from background thread and read from UI thread without synchronization -- data race with crash potential
- `isGenerating` (`bool`) same issue, though benign on x86
- Both generation paths use `std::thread(...).detach()` with captured `this` pointer -- use-after-free if editor closed during generation

**Impact:** Barrier handling is now fully fixed for both paths. Thread safety issues are latent (sporadic crash risk during generation, not affecting output correctness).

### Issue 7: OpenSeaMap Integration Is Underutilised

OpenSeaMap (`map.openseamap.org`) has rich global seamark data via Overpass API, but the current integration:

- Only queries **nodes**, missing ways/relations (harbour polygons, fairway boundaries)
- Only fetches buoys, lights, and landmarks -- missing beacons, fog signals, racon, wrecks, piers
- Never supplements S-57 data (mutually exclusive paths)
- Doesn't extract light properties from `man_made=lighthouse` nodes

**Impact:** When no S-57 chart is available, generated worlds are missing ~30% of available navigation aids. Even with S-57, supplementary data (buildings, structures) is not merged.

### Issue 8: Natural Earth Coastlines Are Too Coarse for World Generation

**Status: Partially fixed (2026-02-20). OSM land polygons replace NE for land/water classification. Flat 2.0m land height remains -- see Issue 10.**

The no-chart fallback path (Path B) relied on **Natural Earth** static binary files for land/water classification. These miss small islands and have inaccurate coastlines.

**Fix applied:** Three-tier land classification system:

1. **OSM land polygons** (shapefile via GDAL, `OSMLandPolygons` class) -- sub-metre accuracy, best option
2. **Natural Earth + Overpass island supplement** (`place=island|islet` ways) -- catches small islands NE misses
3. **Natural Earth only** -- minimum fallback

**Current data architecture (no-chart path):**

| Data | Source | Quality |
| --- | --- | --- |
| Land/water classification | OSM land polygons shapefile (1.3GB) | Excellent |
| Land/water fallback | Natural Earth + Overpass island supplement | Good |
| Land elevation | AWS Terrain Tiles (Terrarium PNG) | Good -- see task 0.10 |
| Ocean depth | AWS Terrain Tiles (ETOPO1 bathymetry) | Good -- see task 0.10 |
| Terrain detail textures | **Single satellite photo, no detail** | Poor -- see Issue 11 |
| Building material variation | **Single procedural brick, no variation** | Poor -- see Issue 11 |
| Buoys, lights, beacons | OpenSeaMap (Overpass) | Good |
| Buildings, piers, breakwaters | OSM (Overpass) | Good |
| Barriers (dams) | OSM (Overpass) | Good |

**Remaining impact:** Coastline accuracy and elevation are now excellent (Issues 8, 10 fixed). Remaining gap: terrain uses a single satellite photo with no close-range detail textures, and buildings share one procedural material with no variation. See Issue 11.

### Issue 10: No Real Elevation Data in No-Chart Generation Path

**Status: Fixed (2026-02-20). AWS Terrain Tiles integrated. Blur removed. See task 0.10.**

The no-chart path sets all land to 2.0m and all water to -20.0m. An 8-pass 5x5 box blur (~11px effective radius) creates gradual slopes at coastlines to avoid 22m cliff faces. This blur causes cascading problems:

- Erodes thin barriers (dams, breakwaters) back below sea level
- Shrinks islands by ~11 pixels per edge
- Weakens narrow headland connections
- Requires post-blur land clamping workaround
- Produces unrealistic flat terrain with no hills, valleys, or real bathymetry

**Meanwhile, the S-57 chart path** (`HeightmapGenerator`) already supports:

- `loadDEMTiles()` -- Copernicus DEM GeoTIFFs for real land elevation
- `loadBathymetryTiles()` -- BlueTopo bathymetry GeoTIFFs
- IDW interpolation from S-57 soundings
- `GEBCOReader` -- global bathymetry (implemented, **never used**)

None of this infrastructure is wired into the no-chart path.

**Available free elevation data sources (researched Feb 2026):**

| Source | Resolution | Land | Bathy | Free HTTP Tiles | Auth | GDAL | Best For |
| --- | --- | --- | --- | --- | --- | --- | --- |
| **AWS Terrain Tiles** | ~30m (zoom 15) | Yes | Yes (ETOPO1) | YES (S3 PNG z/x/y) | None | Decode PNG | On-the-fly download |
| **Copernicus DEM 30m** | 30m | Yes | No | YES (S3 COG 1x1 deg) | None | YES | High-quality land |
| **Copernicus DEM 90m** | 90m | Yes | No | YES (S3 COG 1x1 deg) | None | YES | Lighter-weight land |
| **ETOPO 2022** | ~450m | Yes | Yes | YES (ERDDAP API) | None | YES | Combined topo+bathy API |
| **GEBCO 2025** | ~450m | Yes | Yes | Partial (OPeNDAP) | None | YES | Best bathymetry quality |
| **SRTM/NASADEM** | 30m | Yes | No | Via 3rd party | Varies | YES | Legacy, superseded |
| **ASTER GDEM v3** | 30m | Yes | No | Via OpenTopo API | API key | YES | Polar coverage |
| **OpenTopography API** | Various | Yes | Partial | YES (REST) | API key | N/A | Flexible subsetting |

#### Recommended approach: AWS Terrain Tiles (Terrarium PNG)

Best fit because it mirrors the existing ESRI satellite tile infrastructure exactly:

- Same z/x/y slippy map URL pattern: `https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png`
- No authentication, no API key, no rate limits
- Includes both land elevation AND ocean bathymetry in one source
- Small tiles (~20-80 KB PNG at 256x256 pixels)
- stb_image already available for PNG decoding
- Encoding: `elevation_m = (R * 256 + G + B / 256) - 32768` (nearly identical to BC's own height encoding)
- Zoom 15 = ~30m/pixel, zoom 10 = ~150m/pixel -- matches heightmap resolution needs
- EU mirror available (`elevation-tiles-prod-eu`)
- Caches alongside satellite tiles in `%APPDATA%/Bridge Command/tilecache/`
- Sources: SRTM 30m (land 60N-60S), 3DEP 3-10m (US), GMTED2010, EU-DEM, ETOPO1 (ocean), ArcticDEM, regional LiDAR

With real elevation data, the 8-pass blur becomes unnecessary. Natural terrain provides gradual slopes at coastlines. This eliminates the blur-induced barrier erosion, island shrinkage, and headland weakening problems entirely.

**Impact:** Flat terrain makes generated worlds unrealistic. The blur workaround causes cascading data integrity problems. Real elevation data would solve both issues simultaneously.

### Issue 11: Terrain and Building Textures Are Flat and Unrealistic

Both terrain and building textures lack close-range detail. Terrain uses a single satellite photo stretched over the entire heightmap -- no grass, rock, sand, or cliff detail. Buildings use a single procedurally generated brick pattern (512x512 `building_wall.png`) with no material variation.

**Current terrain texturing:**
- Single `texture.png` (satellite imagery from ESRI) draped over terrain mesh
- Both Irrlicht (`BCTerrainSceneNode`) and WickedEngine (`WickedTerrainNode`) use one `BASECOLORMAP`
- No detail textures, no splatmaps, no noise-based variation
- At close range (~100m), terrain looks blurry/pixelated

**Current building texturing:**
- Procedural brick wall pattern + slate roof tiles (hash-based noise)
- Two materials: `building_wall` and `building_roof` with PBR properties (roughness, metalness)
- All buildings share identical texture -- no variation, no weathering

**WickedEngine terrain capabilities (unused):**

WickedEngine has a **built-in 4-region terrain blending system** (`SHADERTYPE_PBR_TERRAINBLENDED`) with virtual texturing:

| Region | Weight Source | Suggested Material |
|---|---|---|
| `region_base` | `1.0 - slope` (flat areas) | Grass/field |
| `region_slope` | `slope` (steep areas) | Rock/cliff |
| `region_low` | Height below zero | Sand/mud |
| `region_high` | Height above zero | Grass/forest |

Auto-generated splatmaps from terrain topology, compute shader blending, BC1/BC3/BC5 compression, virtual texture atlas. Supports tri-planar mapping for cliffs.

**Two-phase approach:**

Phase A (immediate, CPU-side): Pre-blend detail textures into `texture.png` during world generation. Combine satellite imagery with noise-modulated terrain type textures (grass, rock, sand) based on elevation, slope, and water proximity. No renderer changes needed.

Phase B (later, GPU-side): Wire up WickedEngine's native terrain blending. Generate a splatmap during world generation. Supply 4 PBR material textures. Let the GPU shader handle blending, tri-planar mapping, and detail tiling.

**Free PBR texture sources (all CC0):**

| Source | URL | Content |
|---|---|---|
| ambientCG | ambientcg.com | 2000+ materials: ground, grass, gravel, rock, concrete, brick |
| Poly Haven | polyhaven.com/textures | Photoscanned 1K-8K: grass, rock, sand, dirt, snow |
| 3D Textures | 3dtextures.me | Ground, grass, rock, sand, snow with full PBR maps |
| freePBR | freepbr.com | Ground, cliff, rock, grass, concrete, metal |

**Impact:** Generated worlds look flat and artificial at close range. Buildings lack visual variety. These are the most visible remaining quality gaps after elevation data was fixed.

### Issue 9: No HTTP Client Abstraction

At least 4 independent HTTP implementations exist: `OSMBuildingReader::httpPost()`, `OpenSeaMapSource`, `OSMWaterReader`, and `TileDownloader`. Each has its own URL parsing, error handling, timeout, and retry logic. The fallback-server pattern is duplicated across multiple classes.

**Impact:** Adding logging, rate limiting, proxy support, or changing the HTTP library requires changes in 4+ places.

---

## 5. Development Approach: TDD

All tasks below follow a strict test-driven development discipline:

1. **Write a failing test first** that defines the expected behaviour
2. **Implement the minimum code** to make the test pass
3. **Refactor** while keeping tests green
4. **Run the full test suite** before committing

### Test Infrastructure

- **Framework:** Catch2 v3.5.2 via CMake FetchContent
- **Build:** `cmake --build . --target bc-tests` from `src/tests/`
- **Static library:** `bc-testable` contains domain logic with zero Irrlicht runtime dependency
- **GDAL tests:** Conditionally compiled when `GDAL_FOUND` (chart reader, heightmap, world generator)

### Test Conventions

- Tag tests with component name: `[heightmap]`, `[worldgen]`, `[osmwater]`, `[openseamap]`
- Use small synthetic data (5x5 grids, 3-point polygons) -- no external files or network calls
- Mock HTTP responses by injecting JSON strings directly into parse methods
- Each fix must have at least one regression test that fails without the fix

---

## 6. Task Breakdown

Tasks are organized into phases. Each phase builds on the previous. Tasks are granular enough for a junior developer to pick up individually.

### Phase 0: World Generation Fixes (Immediate)

**Goal:** Fix barrage/harbour generation, improve coastline accuracy, enable OpenSeaMap as a supplementary data source.

#### 0.1 Add barrier support to HeightmapGenerator (TDD) -- DONE (2025-02-19)

- **Status:** Complete. `setBarriers()` + `applyBarrierFloodFill()` added. IEEE 754 `-0.0f` bug fixed (`<= 0.0f`). 8 barrier tests passing (box, closed polygon, headland+barrier, L-shape, -0.0f edge case, no-barrier, non-spanning).
- **Files modified:** `src/HeightmapGenerator.hpp/cpp`, `src/tests/test_heightmap_generator.cpp`

#### 0.2 Add dam/breakwater queries to OSMWaterReader (TDD) -- DONE (2025-02-19)

- **Status:** Complete. `waterway=dam` and `man_made=breakwater` queries added. `getBarriers()` accessor returns parsed barrier geometries. 8 tests passing (dam, breakwater, separation, closed polygon, multiple barriers, empty, malformed, single-point).
- **Files modified:** `src/editor/OSMWaterReader.hpp/cpp`, `src/tests/test_osm_water_reader.cpp`

#### 0.3 Wire barrier flood-fill into WorldGenerator -- DONE (2025-02-19)

- **Status:** Complete for S-57 chart path. `WorldGenerator::generateWorld()` now calls `waterReader.getBarriers()` and passes them to `hmGen.setBarriers()`. S-57 shoreline constructions also converted to barrier geometries.
- **Files modified:** `src/WorldGenerator.cpp`
- **NOTE:** This only fixes the S-57 chart path. The no-chart fallback path in `EditorApp.cpp:2181-2511` has its own inline barrier code that was NOT updated. See Issue 6 for details.

#### 0.3a Extract barrier flood-fill into standalone utility -- DONE (2026-02-19)

- **Status:** Complete. Created `BarrierFloodFill.hpp/cpp` (no GDAL dependency) with the shared algorithm. `HeightmapGenerator::applyBarrierFloodFill()` now delegates to it (flatten 2D grid, call, unflatten). `EditorApp.cpp` fallback path replaced ~110 lines of inline barrier code with a single `BarrierFloodFill::apply()` call. Also fixed a latent bug where barrier boundary pixels were getting `reclaimHeight` instead of `barrierHeight` (ordering issue in the reclaim loop). 8 standalone barrier tests added (box, no-barrier, non-enclosing, -0.0f, land preservation, null grid, custom heights, headland+barrier). Total: 212 tests passing (with GDAL), 147 without.
- **Files created:** `src/BarrierFloodFill.hpp`, `src/BarrierFloodFill.cpp`, `src/tests/test_barrier_flood_fill.cpp`
- **Files modified:** `src/HeightmapGenerator.cpp`, `src/editor/EditorApp.cpp`, `src/Visual Studio solution/bridgecommand-ed.vcxproj`, `src/tests/CMakeLists.txt`

#### 0.3b Add Overpass timeout retry and user feedback -- DONE (2025-02-19)

- **Status:** Complete. Barrier query now retries with fallback Overpass server (`overpass.kumi.systems`), increased timeout (90s/120s), logs errors to stderr, shows visible warning when barriers unavailable.
- **Files modified:** `src/editor/EditorApp.cpp`

#### 0.3c Fix smoothing blur eroding barrier heights -- DONE (2026-02-20)

- **Status:** Complete. The 8-pass 5x5 box blur ran AFTER barrier flood-fill, averaging thin barrier lines (1-3px, 2.0m) against surrounding deep water (-20.0m) until barriers were eroded below sea level. Also affected reclaimed area edges. Fixed by moving blur BEFORE flood-fill -- smooths NE coastline transitions (its purpose) while preserving crisp barrier boundaries.
- **Files modified:** `src/editor/EditorApp.cpp`

#### 0.3d Fix OSMBuildingReader barrier cache and cleanup -- DONE (2026-02-20)

- **Status:** Complete. Three fixes: (1) `barrierLines` not cleared in `query()` -- would cause phantom barriers if reader reused. (2) `barrierLines` not cleared in `loadCache()`. (3) `saveCache()`/`loadCache()` now serialize barrier lines in a `BARRIERS N` section appended after buildings (backwards compatible with older cache files).
- **Files modified:** `src/editor/OSMBuildingReader.cpp`

#### 0.3e Consolidate barrier query into building query -- DONE (2026-02-20)

- **Status:** Complete. The separate Overpass barrier query (often rate-limited) was eliminated. Barrier data (dams, breakwaters) is now extracted from the building query response via `OSMBuildingReader::getBarrierLines()`. Building query moved before heightmap generation so barrier data is available. Reduced Overpass requests from 3 to 2.
- **Files modified:** `src/editor/EditorApp.cpp`, `src/editor/OSMBuildingReader.hpp`, `src/editor/OSMBuildingReader.cpp`

#### 0.3f Add vertex snapping for closed polygon barriers -- DONE (2026-02-20)

- **Status:** Complete. Cardiff Bay Barrage is a closed polygon (first == last vertex). Original endpoint snapping only checked `front()`/`back()` which are identical for closed polygons, so only one end got connected to land. Fixed: snapping now checks ALL vertices of each barrier polyline. Any vertex on water within 20px of land gets a connection line drawn to nearest land pixel. 2 new tests added (endpoint snapping, already-on-land). Total: 149 tests, 10610 assertions.
- **Files modified:** `src/BarrierFloodFill.cpp`, `src/tests/test_barrier_flood_fill.cpp`

#### 0.8 Replace Natural Earth with OSM land polygons -- DONE (2026-02-20)

- **Status:** Complete (Phase A + Phase B both implemented). Three-tier land classification:

  1. OSM land polygons shapefile (`OSMLandPolygons` class, GDAL rasterization) -- best quality
  2. Natural Earth + Overpass island supplement (`place=island|islet` scanline fill) -- fallback
  3. Natural Earth only -- minimum

- **Files created:** `src/editor/OSMLandPolygons.hpp`, `src/editor/OSMLandPolygons.cpp`
- **Files modified:** `src/editor/EditorApp.cpp`, `src/editor/OSMBuildingReader.hpp/cpp`, `src/Visual Studio solution/bridgecommand-ed.vcxproj`, `src/tests/CMakeLists.txt`
- **Data:** `bin/Data/land-polygons-split-4326/` (1.3GB shapefile from osmdata.openstreetmap.de)
- **Also added:** Land clamping after blur (save pre-blur land mask, clamp confirmed-land pixels to min 1.0f). Barrier snap radius increased from 20 to 40 pixels. Island polygon cache serialization (ISLANDS section in building cache).
- **Remaining:** Land classified as flat 2.0m, water as flat -20.0m. Real elevation data needed -- see task 0.10.

#### 0.10 Integrate real elevation data via AWS Terrain Tiles -- DONE (2026-02-20)

- **Status:** Complete. `ElevationTile` class downloads Terrarium PNG tiles from AWS S3, decodes to float elevation, composites with bilinear resampling. Integrated into no-chart generation path. Blur removed when elevation data available (kept as fallback if download fails).
- **Files created:** `src/editor/ElevationTile.hpp`, `src/editor/ElevationTile.cpp`
- **Files modified:** `src/editor/EditorApp.cpp`, `src/Visual Studio solution/bridgecommand-ed.vcxproj`, `src/tests/CMakeLists.txt`
- **How it works:**

  1. Land/water classification from OSM land polygons (binary mask, sub-metre accuracy)
  2. Download Terrarium PNG tiles from `s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png`
  3. Decode RGB to float: `elevation_m = R * 256 + G + B / 256 - 32768`
  4. Merge: land mask pixel gets `max(elevation, 0.5m)`, water pixel gets `min(elevation, -0.5m)`
  5. No blur needed -- real terrain has natural gradients
  6. terrain.ini TerrainMaxHeight/SeaMaxDepth now computed from actual data (+ 10% margin)

- **Fallback:** If elevation tile download fails, reverts to flat 2.0m/-20.0m with 8-pass blur.
- **Cache:** `%APPDATA%/Bridge Command/tilecache/elevation/` (same structure as satellite tiles).

#### 0.10a Future elevation enhancements

- **Copernicus DEM 30m** via AWS S3 COG GeoTIFFs for higher-quality land elevation. Anonymous S3, 1x1 degree tiles (~13 MB), GDAL reads natively. Could use GDAL `/vsicurl/` for HTTP range reads (download only the needed portion). No bathymetry -- would combine with Terrain Tiles for underwater.
- **ETOPO 2022** via ERDDAP API for improved bathymetry (15 arc-second, ~450m). Free programmatic subsetting: `coastwatch.pfeg.noaa.gov/erddap/griddap/ETOPO_2022_v1_15s.geotif?z[(lat):(lat)][(lon):(lon)]`. GDAL reads returned GeoTIFF.
- **GEBCO 2025** reader already exists (`GEBCOReader.hpp/cpp`, fully implemented, currently unused). Could be wired into the no-chart path as a high-quality bathymetry source. 15 arc-second resolution, requires pre-download (~7.5 GB).
- **Wire existing `HeightmapGenerator.loadDEMTiles()`** into the no-chart path. Already supports Copernicus DEM + BlueTopo bathymetry with bilinear sampling, but currently only available via `WorldGenerator::generateWorld()` (S-57 chart path).

#### 0.11 Terrain detail texturing -- Phase A (CPU-side pre-blending)

- **Files:** Modify `src/editor/EditorApp.cpp`, create `src/editor/TerrainTextureBlender.hpp/cpp`
- **What:** During world generation, enhance `texture.png` by blending satellite imagery with tileable detail textures based on terrain properties. For each pixel, compute terrain type weights from:
  - **Slope** (from heightmap gradient): steep = rock/cliff, flat = grass/field
  - **Elevation**: low near water = sand/beach, mid = grass, high = rock/snow
  - **Water proximity** (BFS distance from coastline): 0-30m = wet sand, 30-100m = dry sand/scrub, 100m+ = grass/forest
  - **Noise** (Perlin/Simplex FBM): break up transitions, add micro-variation
- **Detail textures:** Ship 4-6 tileable textures with the editor (256x256 each, embedded or downloaded from ambientCG/Poly Haven, CC0). Grass, rock, sand, dirt minimum. Tile at ~10m repeat, with noise-modulated blending to hide repetition.
- **Blending formula:** `finalColor = lerp(satelliteColor, detailColor, detailWeight * noiseModulation)` where detailWeight is strongest at close-range terrain scale and weakest at satellite-dominant distance scale.
- **Acceptance:** Generated `texture.png` shows visible grass/rock/sand detail. Close-range terrain no longer appears as blurry satellite photo.

#### 0.12 Building material variation and weathering

- **Files:** Modify `src/editor/EditorApp.cpp` (building texture generation, lines ~2733-2854)
- **What:** Improve procedural building textures with:
  - **Multiple wall types**: brick (current), concrete (flat grey + stain noise), stone (irregular block pattern), render/stucco (smooth + vertical streaks). Select per-building based on hash of position.
  - **Per-building color tinting**: multiply base texture by a per-building tint derived from position hash. Range: warm cream to cool grey, subtle (0.85-1.15 per channel).
  - **Weathering gradient**: darken toward base (moisture wicking), add noise-driven stains. `albedo *= 1.0 - verticalGradient * 0.2 - stainNoise * 0.1`
  - **Roof variation**: add tile color variation (terracotta, grey slate, dark tile) selected per-building.
- **Acceptance:** Buildings in generated worlds show visible variation in material, color, and weathering.

#### 0.11a Terrain detail texturing -- Phase B (WickedEngine GPU-side)

Future enhancement. Wire up WickedEngine's native `SHADERTYPE_PBR_TERRAINBLENDED` with 4-region splatmap:

- Generate a splatmap (RGBA PNG) during world generation alongside height.png and texture.png
- Supply 4 PBR material texture sets (albedo + normal + roughness per region)
- Regions: base=grass (flat land), slope=rock (cliffs), low=sand (near water), high=scrub/forest
- WickedEngine handles compute shader blending, virtual texturing, tri-planar mapping for cliffs
- Requires modifying `WickedTerrainNode` to load splatmap + region materials instead of single BASECOLORMAP
- Also add FBM noise in fragment shader for macro variation (large-scale color drift) and micro detail (close-range anti-tiling)

#### 0.9 Fix thread safety in world generation

- **Files:** Modify `src/editor/EditorApp.hpp`, `src/editor/EditorApp.cpp`
- **What:** `generateStatus` (`std::string`) is written from background thread and read from UI thread without synchronization -- data race with crash potential. `isGenerating` (`bool`) same issue. Both generation paths use `std::thread(...).detach()` with captured `this` -- use-after-free if editor closed during generation. Fix: protect `generateStatus` with `std::mutex`, make `isGenerating` `std::atomic<bool>`, replace `detach()` with stored `std::thread` joined on destruction.
- **Acceptance:** No thread sanitizer warnings during world generation.

#### 0.4 Add satellite texture option to chart-based generation (TDD)

- **Test first:** Verify `SatelliteTexture` can composite a 2x2 tile grid into an RGB buffer (unit test with mock tile data).
- **Files:** Modify `src/WorldGenerator.cpp`, `src/WorldGenerator.hpp`
- **What:** Add an optional `useSatelliteTexture` parameter. When true, use `SatelliteTexture` to download ESRI tiles for the chart bounds and write as `texture.png` instead of the colour-coded elevation texture.
- **Acceptance:** Generated world uses satellite imagery when the option is set.

#### 0.5 Extract lighthouse light properties from OpenSeaMap (TDD)

- **Test first:** Inject JSON with a `man_made=lighthouse` node that also has `seamark:light:character=Fl` tags. Assert both an `OsmLandmark` AND an `OsmLight` are created.
- **Files:** Modify `src/editor/OpenSeaMapSource.cpp`
- **What:** When parsing `man_made=lighthouse` nodes, also check for `seamark:light:*` tags. If present, create an `OsmLight` entry in addition to the `OsmLandmark`.
- **Acceptance:** Test passes. Lighthouses generate both landmark and light INI entries.

#### 0.6 Elevate OpenSeaMap to supplementary data source

- **Files:** Modify `src/editor/EditorApp.cpp`, `src/WorldGenerator.cpp`
- **What:** When S-57 chart is loaded, still query OpenSeaMap for seamark types NOT in the chart (beacons, harbour structures). Merge OpenSeaMap buoys/lights/landmarks with chart-extracted data, deduplicating by proximity (50m threshold). Add a UI checkbox "Supplement with OpenSeaMap data" (default: on).
- **Acceptance:** A chart-based world with OpenSeaMap enabled has more navigation aids than chart-only.

#### 0.7 Add way/relation queries to OpenSeaMap source

- **Files:** Modify `src/editor/OpenSeaMapSource.cpp`
- **What:** Extend the Overpass query to also fetch `way["seamark:type"]` and `relation["seamark:type"]` with `out geom center;`. Parse harbour areas, fairway boundaries, and pier geometries from ways. Use centroids for point features, full geometry for area features.
- **Acceptance:** Harbour areas and piers appear in generated worlds.

### Phase 1: Decouple Simulation from Rendering

**Goal:** Make all simulation features available to any renderer.

#### 1.1 Extract FrameState struct
- **File:** Create `src/FrameState.hpp`
- **What:** Define a renderer-agnostic struct containing everything a renderer needs per frame: camera position/orientation, ship positions/headings, buoy positions, light states, weather (wind, rain, visibility, cloud), time of day, water state, radar image data (raw pixel buffer), HUD data (speed, heading, depth, rudder angle, engine RPM).
- **Why:** This becomes the contract between simulation and rendering.
- **Acceptance:** Struct compiles with zero Irrlicht includes.

#### 1.2 Extract WeatherSystem from SimulationModel
- **Files:** Create `src/WeatherSystem.hpp/cpp`
- **What:** Move all weather state (wind speed/direction, rain density, visibility, fog, tidal stream, weather override flags) and methods (`setWeather()`, `getWeather()`, `setRain()`, etc.) from `SimulationModel` into a standalone `WeatherSystem` class.
- **Why:** SimulationModel has 150+ methods; extracting subsystems reduces coupling.
- **Acceptance:** `SimulationModel` delegates to `WeatherSystem`. All existing tests pass. No Irrlicht types in `WeatherSystem`.

#### 1.3 Extract TimeManager from SimulationModel
- **Files:** Create `src/TimeManager.hpp/cpp`
- **What:** Move `scenarioTime`, `absoluteTime`, `deltaTime`, `accelerator`, `timeOffset`, and related methods into `TimeManager`.
- **Acceptance:** Time calculations have no Irrlicht dependency.

#### 1.4 Extract CameraManager from SimulationModel
- **Files:** Create `src/CameraManager.hpp/cpp`
- **What:** Move camera state (position, look angle, view angle, zoom, view mode), camera switching logic, and viewport calculation into `CameraManager`. Use `bc::graphics::Vec3` and `bc::graphics::Quaternion` instead of Irrlicht types.
- **Acceptance:** CameraManager produces camera parameters consumable by either renderer.

#### 1.5 Extract RadarSystem from SimulationModel
- **Files:** Create `src/RadarSystem.hpp/cpp`
- **What:** Move `RadarCalculation`, radar image buffers, ARPA tracking, EBL/VRM state from SimulationModel. Replace `irr::video::IImage*` with a renderer-agnostic pixel buffer (e.g., `std::vector<uint32_t>`). `RadarCalculation` already does math in its own types; the coupling is only in the image output.
- **Acceptance:** RadarSystem produces a raw RGBA pixel buffer. Either renderer can upload it as a texture.

#### 1.6 Make SimulationModel produce FrameState
- **Files:** Modify `src/SimulationModel.hpp/cpp`, `src/SimulationBridge.hpp/cpp`
- **What:** Add `FrameState SimulationModel::getFrameState()` method that populates the FrameState struct from current simulation state. Extend SimulationBridge to expose this.
- **Acceptance:** WickedMain.cpp can call `bridge->getFrameState()` and get everything needed for rendering.

#### 1.7 Remove Irrlicht types from OwnShip navigation state
- **Files:** Modify `src/OwnShip.hpp/cpp`
- **What:** Replace `irr::core::vector3df` position/rotation storage with `bc::graphics::Vec3`. Keep Irrlicht scene node pointer only in a rendering-specific section (or behind `#ifdef`). The physics and navigation state must be pure C++.
- **Acceptance:** `OwnShip::getPosition()`, `getHeading()`, `getSpeed()` return Irrlicht-free types.

#### 1.8 Remove Irrlicht types from OtherShip position state
- **Files:** Modify `src/OtherShip.hpp/cpp`, `src/OtherShips.hpp/cpp`
- **What:** Same as 1.7 but for traffic vessels. Separate scene node (rendering) from position/heading/speed (simulation).
- **Acceptance:** OtherShip position data is Irrlicht-free.

---

### Phase 2: Complete WickedEngine Integration

**Goal:** WickedEngine path has feature parity with Irrlicht.

#### 2.1 Consume FrameState in WickedMain
- **Files:** Modify `src/WickedMain.cpp`
- **What:** Replace all direct SimulationBridge getters with a single `getFrameState()` call. Remove the parallel camera state globals (lines 118-131). Use FrameState for all ship positions, weather, camera.
- **Acceptance:** WickedMain renders correctly using only FrameState data.

#### 2.2 Implement radar texture upload in WickedEngine
- **Files:** Modify `src/WickedMain.cpp`, add radar overlay rendering
- **What:** Take the raw RGBA radar pixel buffer from RadarSystem, upload it as a WickedEngine texture each frame, render it as a screen-space quad (small or full-screen based on mode).
- **Acceptance:** Radar display visible in WickedEngine path.

#### 2.3 Implement HUD overlay in WickedEngine
- **Files:** Modify `src/WickedMain.cpp` ImGui overlay
- **What:** Use ImGui (already integrated in WickedEngine path) to render speed, heading, depth, rudder angle, engine RPM, rate of turn, GPS position. Style to match maritime instrument aesthetics.
- **Acceptance:** All navigation data visible on screen in WickedEngine path.

#### 2.4 Implement instrument controls in WickedEngine
- **Files:** Modify `src/WickedMain.cpp` ImGui overlay
- **What:** Add ImGui-based helm wheel, engine telegraph sliders, thruster controls. Wire these back through SimulationBridge to set engine/rudder commands.
- **Acceptance:** Ship can be steered from WickedEngine path.

#### 2.5 Wire NMEA output to WickedEngine path
- **Files:** Modify `src/WickedMain.cpp`
- **What:** NMEA output is currently driven from main.cpp's Irrlicht loop. Extract NMEA generation into a standalone component that reads from SimulationModel (not from rendering). Call it from both paths.
- **Acceptance:** OpenCPN receives position data when using WickedEngine.

#### 2.6 Wire networking to WickedEngine path
- **Files:** Modify `src/WickedMain.cpp`
- **What:** NetworkPrimary/NetworkSecondary are already Irrlicht-free in their core logic. Instantiate the correct Network implementation in the WickedEngine path based on operating mode. Call `sendString()` / `listenForMessages()` in the WickedEngine frame loop.
- **Acceptance:** Secondary displays can connect to a WickedEngine primary.

#### 2.7 Wire joystick input to WickedEngine path
- **Files:** Modify `src/WickedMain.cpp`
- **What:** Joystick input currently uses Irrlicht's `SJoystickInfo` and `SEvent::SJoystickEvent`. Replace with direct platform API (XInput on Windows, or SDL2 GameController). Map joystick axes/buttons to engine/rudder/thruster commands via SimulationBridge.
- **Acceptance:** Physical helm hardware works with WickedEngine path.

#### 2.8 Wire sound to WickedEngine path
- **Files:** Modify `src/WickedMain.cpp`
- **What:** `ISound` is already abstracted. Create and update the sound instance from the WickedEngine frame loop using FrameState data (engine RPM for pitch, position for 3D audio).
- **Acceptance:** Engine sounds play in WickedEngine path.

---

### Phase 3: Multi-View and Multi-Monitor

**Goal:** One PC drives 3 displays with synchronized, efficient rendering.

#### 3.1 Implement WickedEngine multi-window rendering
- **Files:** Modify `src/graphics/wicked/WickedMultiView.hpp/cpp`, `src/WickedMain.cpp`
- **What:** `WickedMultiView` already has swap chain infrastructure for multiple windows. Connect it to the simulation: create N windows (one per configured monitor), each with a different camera look angle, all sharing the same scene. Render all views from a single WickedEngine instance (shared GPU resources, one scene graph, multiple cameras).
- **Acceptance:** 3 windows on 3 monitors show port/center/starboard views from one process with synchronized frames.

#### 3.2 Add multi-view configuration UI
- **Files:** Create `src/SetupWizard.hpp/cpp` or add to launcher
- **What:** ImGui-based setup screen that detects available monitors (Win32 `EnumDisplayMonitors`), lets the user assign each monitor a role (bridge window with look angle, radar, instruments), and generates the appropriate configuration. Save to `bc5.ini`.
- **Acceptance:** User can set up 3-TV bridge from a GUI without editing INI files.

#### 3.3 Borderless fullscreen per-monitor in WickedEngine
- **Files:** Modify `src/WickedMain.cpp`
- **What:** Create borderless windows sized and positioned to exactly cover each assigned monitor. Handle `WM_DPICHANGED` correctly (already partially implemented).
- **Acceptance:** Each TV shows a seamless fullscreen view with no title bar or taskbar.

#### 3.4 Frame synchronization across views
- **Files:** Modify `src/graphics/wicked/WickedMultiView.hpp/cpp`
- **What:** Ensure all swap chains present within the same vsync interval. Use a shared fence or barrier. Without this, different monitors may show different simulation timestamps.
- **Acceptance:** No visible tearing or temporal offset between adjacent bridge windows.

---

### Phase 4: Instrument Station

**Goal:** Second PC shows helm, radar, and chart viewer with modern UI.

#### 4.1 Modernize repeater with ImGui
- **Files:** Rewrite `src/repeater/main.cpp`
- **What:** Replace Irrlicht GUI with ImGui (Win32+OpenGL, same pattern as editor). Render realistic heading repeater (compass rose), rudder angle indicator (sector gauge), and engine RPM displays using ImGui custom draw commands.
- **Acceptance:** Repeater looks like a real bridge instrument panel, not Windows XP sliders.

#### 4.2 Create standalone radar application
- **Files:** Create `src/radarStation/` directory and project
- **What:** Dedicated radar display application using ImGui. Receives radar pixel data over network from primary. Displays full-screen radar with controls (gain, clutter, range, EBL, VRM, ARPA). Alternatively, extend the repeater app with a radar mode.
- **Acceptance:** Radar runs full-screen on instrument PC with all features.

#### 4.3 Add realistic instrument widgets
- **Files:** Create `src/widgets/CompassRose.hpp`, `EngineOrderTelegraph.hpp`, `RudderAngleIndicator.hpp`, `RateOfTurnIndicator.hpp`
- **What:** Custom ImGui widgets that render photorealistic (or at least skeuomorphic) maritime instruments using ImGui draw lists. Compass rose with degree markings, brass EOT handle, sector-style rudder indicator, analog ROT gauge.
- **Acceptance:** Visual resemblance to real marine instruments. Usable for recognition training.

#### 4.4 Extend network protocol for instrument data
- **Files:** Modify `src/NetworkPrimary.cpp`, `src/NetworkSecondary.cpp`
- **What:** Add a new message type for detailed instrument data: engine order, actual RPM (port/stbd), rudder order vs actual, thruster state, gyro heading vs magnetic heading, GPS quality, AIS contacts. Use a simple versioned format (version byte + key-value pairs, or lightweight binary with field IDs).
- **Acceptance:** Instrument station receives rich data beyond what BC/SC strings currently provide.

#### 4.5 OpenCPN integration documentation and helper
- **Files:** Create launcher configuration for OpenCPN auto-launch
- **What:** Document the NMEA UDP setup step-by-step. Optionally add a launcher button that auto-starts OpenCPN with the correct connection configuration (if OpenCPN is installed). Verify NMEA sentences: GGA (position), RMC (recommended minimum), HDT (true heading), VTG (track/speed), DBT (depth), MWV (wind).
- **Acceptance:** User clicks one button and OpenCPN shows ship position on real charts.

#### 4.6 Add Signal K output option
- **Files:** Create `src/SignalKOutput.hpp/cpp`
- **What:** Output simulation state as Signal K JSON over WebSocket. This allows multiple consumers (OpenCPN, web dashboards, phone apps) to subscribe to live data. Signal K is the modern open standard, complementing NMEA 0183.
- **Acceptance:** Signal K server can connect and display vessel data.

---

### Phase 5: Visual Fidelity

**Goal:** Approach photorealistic maritime visuals.

#### 5.1 Parameterize WickedEngine ocean to Beaufort scale
- **Files:** Modify WickedEngine ocean integration in `src/WickedMain.cpp`
- **What:** Map Beaufort wind force (0-12) to FFT ocean parameters: wave amplitude, wavelength, choppiness, foam threshold. Use published sea state tables (significant wave height, period) as reference.
- **Acceptance:** Setting wind to Beaufort 6 produces visually convincing rough seas.

#### 5.2 Implement ship wake rendering
- **Files:** Add wake particle/mesh system in WickedEngine path
- **What:** Render Kelvin wake pattern behind moving ships. Can be done with animated mesh deformation or particle-based foam. Scale with ship speed and hull type.
- **Acceptance:** Moving ships produce visible wake.

#### 5.3 Implement maritime fog and visibility
- **Files:** Modify WickedEngine atmosphere/fog settings
- **What:** Parameterize fog density from visibility distance in nautical miles. Support different fog types: sea fog (low-lying), rain (uniform), haze. Tie to weather system visibility parameter.
- **Acceptance:** Setting visibility to 0.5nm produces dense fog. Lights and landmarks appear/disappear at correct range.

#### 5.4 Improve coastal/shoreline rendering
- **Files:** Modify terrain rendering in WickedEngine path
- **What:** Add shoreline wave interaction (foam line at water/terrain boundary). Add procedural vegetation (trees, grass) using WickedEngine's built-in grass system. Terrain texture detail is handled by tasks 0.11/0.11a.
- **Acceptance:** Coastline looks natural from bridge distance (0.5-5nm).

#### 5.5 Implement day/night cycle lighting
- **Files:** Modify WickedEngine lighting
- **What:** Map simulation time to sun position (using lat/lon + date for solar angle). Transition sky, ambient light, and water color through dawn/day/dusk/night. Enable navigation light rendering at night (green/red/white lights on ships, buoy lights with correct flash patterns).
- **Acceptance:** Night passage with visible navigation lights.

#### 5.6 Add building/structure generation from OSM
- **Files:** Extend world generation pipeline
- **What:** Download OSM building footprints for the chart area. Extrude to estimated heights. Generate simple textured buildings. Place in the world as landmarks visible from sea.
- **Acceptance:** Generated worlds show recognizable coastal settlements.

#### 5.7 Improve satellite texture resolution
- **Files:** Modify `src/editor/SatelliteTexture.cpp`
- **What:** Currently composites tiles at a fixed zoom level. Support higher zoom for the immediate area and lower zoom for distance. Generate mipmapped textures.
- **Acceptance:** Terrain texture is crisp at close range, doesn't hit API rate limits.

---

### Phase 6: Network Protocol and Online Multiplayer

**Goal:** Robust networking for online games with multiple participants.

#### 6.1 Design versioned binary protocol
- **Files:** Create `src/protocol/Protocol.hpp/cpp`
- **What:** Replace hand-built delimited strings with a structured binary format. Each message starts with: version (uint8), type (uint8), length (uint32), then type-specific payload. Use field tags so receivers can skip unknown fields (forward compatibility).
- **Acceptance:** New clients can talk to old servers and vice versa by ignoring unknown fields.

#### 6.2 Implement protocol serialization/deserialization
- **Files:** Create `src/protocol/Serializer.hpp/cpp`
- **What:** Helper class to serialize/deserialize protocol messages. Support common types: float, double, int32, string, Vec3, arrays. Include CRC or checksum for corruption detection.
- **Acceptance:** Unit tests verify round-trip serialization of all message types.

#### 6.3 Migrate NetworkPrimary to new protocol
- **Files:** Modify `src/NetworkPrimary.cpp`
- **What:** Replace `generateSendString*()` methods with protocol serialization. Maintain backward compatibility by detecting the first byte: if 'B' (ASCII), parse as legacy; if a version byte, parse as new protocol.
- **Acceptance:** New primary works with both old and new secondary clients.

#### 6.4 Migrate NetworkSecondary to new protocol
- **Files:** Modify `src/NetworkSecondary.cpp`
- **What:** Same migration for the receiving side. Auto-detect protocol version.
- **Acceptance:** New secondary works with both old and new primary servers.

#### 6.5 Dynamic buffer sizing
- **Files:** Modify `src/NetworkPrimary.cpp`, `src/NetworkSecondary.cpp`
- **What:** Replace fixed 8KB stack buffers with dynamically sized buffers. Use the length field from the protocol header to allocate exactly what's needed.
- **Acceptance:** Scenarios with many ships don't truncate data.

#### 6.6 Add NAT traversal for online multiplayer
- **Files:** Create `src/network/NATTraversal.hpp/cpp`
- **What:** Implement UDP hole punching or integrate a relay server (TURN). ENet supports some NAT traversal via its connection mechanism, but explicit handling is needed for symmetric NAT. Alternatively, use a lightweight relay service.
- **Acceptance:** Players behind home routers can join a game without manual port forwarding.

#### 6.7 Add authentication for online sessions
- **Files:** Create `src/network/Auth.hpp/cpp`
- **What:** Session-based authentication. Host generates a session code (6-character alphanumeric). Joining players enter the code. No passwords or accounts needed for initial implementation.
- **Acceptance:** Only players with the session code can join.

#### 6.8 Add lobby system to multiplayer hub
- **Files:** Modify `src/multiplayerHub/HubApp.cpp`
- **What:** Before starting a scenario, show a lobby where connected players can see who's joined, select their ship, and ready up. Host can assign roles and start when ready.
- **Acceptance:** Players can join, see each other, and coordinate before scenario starts.

---

### Phase 7: UX and Polish

**Goal:** The application is usable by non-technical users.

#### 7.1 Create setup wizard for multi-station configuration
- **Files:** Add to launcher or create `src/SetupWizard.hpp/cpp`
- **What:** Guided ImGui wizard: (1) detect monitors, (2) assign roles (bridge view, radar, instruments, chart), (3) detect network peers, (4) generate INI files for all stations, (5) offer to launch all processes.
- **Acceptance:** Non-technical user can set up 2-PC bridge in under 5 minutes.

#### 7.2 Remove mandatory 5-second splash screen
- **Files:** Modify `src/main.cpp`, lines 1121-1124
- **What:** Show disclaimer as a quick splash (1 second or "click to continue") or move to an About dialog.
- **Acceptance:** Experienced users can start a scenario in under 3 seconds.

#### 7.3 Add keyboard shortcut overlay
- **Files:** Modify `src/MyEventReceiver.cpp`, add overlay rendering
- **What:** Press F1 or '?' to show a translucent overlay listing all keyboard shortcuts. Group by category (navigation, engine, radar, view).
- **Acceptance:** New users can discover controls without external documentation.

#### 7.4 Unify GUI framework to ImGui
- **Files:** Gradually replace Irrlicht GUI in simulation
- **What:** This is a large task. Start by rendering ImGui on top of the Irrlicht 3D view (Irrlicht already supports custom draw callbacks). Replace one panel at a time: first the data display, then engine controls, then radar controls.
- **Acceptance:** Consistent dark-themed UI across editor and simulation.

#### 7.5 Add DPI awareness to all executables
- **Files:** Add `app.manifest` to all projects, modify window creation
- **What:** Create application manifest with `<dpiAwareness>PerMonitorV2</dpiAwareness>`. Call `SetProcessDpiAwarenessContext()` at startup (already done in WickedMain.cpp). Scale font sizes and widget dimensions by DPI scale factor.
- **Acceptance:** Application renders crisply on Windows 11 with 150% display scaling.

#### 7.6 Add first-run tutorial
- **Files:** Create `src/Tutorial.hpp/cpp`
- **What:** On first launch (detect via config flag), show an interactive walkthrough: explain the launcher buttons, demonstrate starting a simple scenario, show how to control the ship, explain the radar.
- **Acceptance:** A user with no maritime experience can complete a simple scenario within 10 minutes of first launch.

#### 7.7 Improve scenario editor workflow
- **Files:** Modify `src/editor/EditorApp.cpp`
- **What:** Add scenario templates (e.g., "Port approach", "Open sea crossing", "Collision avoidance"). Add tooltips explaining nautical terms. Add a preview mode that shows the scenario in miniature before committing.
- **Acceptance:** A non-mariner can create a basic scenario using a template.

---

### Phase 8: Cross-Platform Hardening

**Goal:** Works on Windows (primary), Linux, macOS.

#### 8.1 Replace Win32 window creation in editor with SDL2/GLFW
- **Files:** Modify `src/editor/EditorApp.cpp`
- **What:** Replace raw `CreateWindowExW`, `SetPixelFormat`, `wglCreateContext` with SDL2 or GLFW calls. ImGui has backends for both. This makes the editor build on Linux and macOS.
- **Acceptance:** Editor compiles and runs on Linux.

#### 8.2 Replace Win32 window creation in multiplayer hub with SDL2/GLFW
- **Files:** Modify `src/multiplayerHub/main.cpp`
- **What:** Same as 8.1 for the multiplayer hub.
- **Acceptance:** Hub compiles and runs on Linux.

#### 8.3 Abstract TileDownloader HTTP backend
- **Files:** Modify `src/editor/TileDownloader.cpp`
- **What:** Already uses WinHTTP on Windows and libcurl on Linux. Verify the libcurl path compiles and works. Add error handling for missing libcurl.
- **Acceptance:** Tile downloads work on Linux.

#### 8.4 Unify build system
- **Files:** Modify `src/CMakeLists.txt`, potentially deprecate some `.vcxproj` complexity
- **What:** Ensure CMake can build all targets on Windows (with MSVC), Linux (with GCC/Clang), and macOS (with Clang). CMake can generate VS solutions, so it can replace manual `.vcxproj` maintenance.
- **Acceptance:** `cmake --build .` produces all executables on all three platforms.

#### 8.5 Resolve x86/x64 platform split
- **Files:** Modify `src/Visual Studio solution/bridgecommand-ed.vcxproj`
- **What:** The editor is x86-only because GDAL is installed for x86. Either install GDAL for x64 via vcpkg (`vcpkg install gdal:x64-windows`) or make GDAL optional at runtime (already partially done with `WITH_GDAL` define). Unifying on x64 eliminates the dual-DLL confusion.
- **Acceptance:** All executables build as x64. No more `Irrlicht.dll` vs `Irrlicht_VS64.dll` confusion.

---

### Priority Order

For the described target setup (3 TVs + instrument station), work in this order:

1. **Phase 0** (world generation fixes) -- Immediately actionable, fixes broken scenarios, no architectural dependency
2. **Phase 1** (decouple simulation) -- Prerequisite for everything else
3. **Phase 2** (complete WickedEngine) -- Gets photorealistic visuals working
4. **Phase 3** (multi-view) -- Enables the 3-TV setup efficiently
5. **Phase 4** (instrument station) -- Makes the second PC useful
6. **Phase 7.1-7.3** (setup wizard, splash, shortcuts) -- Makes it usable
7. **Phase 5** (visual fidelity) -- Ongoing polish
8. **Phase 6** (network protocol) -- Required before online multiplayer
9. **Phase 8** (cross-platform) -- When targeting Linux/macOS users

Phase 0 is independent and can start immediately. Phases 1-4 are the critical path for the multi-station setup. A junior developer should expect each task within a phase to take 1-5 days. Each phase as a whole is 2-6 weeks of focused work.

---

## 7. Bug & Discovery Timeline

Chronological log of bugs found, root causes identified, and fixes applied.

### 2025-02-19: Barrier flood-fill and world generation investigation

**Context:** Cardiff Bay world generation produces no barrage/harbour/marina features.

| Time | Finding | Severity | Status |
| --- | --- | --- | --- |
| AM | Phase 0 tasks 0.1-0.3 implemented: barrier flood-fill added to `HeightmapGenerator`, dam/breakwater queries to `OSMWaterReader`, wired into `WorldGenerator` | -- | Done |
| AM | IEEE 754 `-0.0f` bug: `defaultSeaDepth=0.0` produces `-0.0f`, and `-0.0f < 0.0f` is false. Fixed `HeightmapGenerator` to use `<= 0.0f` | High | Fixed |
| AM | 3 pre-existing test bugs found and fixed: `encodeRGB` wrong expected G value, `toOBJ` wrong material name, `latToTileY` missing output clamping | Low | Fixed |
| AM | All 196 unit tests passing (0 failures) | -- | Done |
| PM | User generates Cardiff Bay world, reports barrage not generated | Critical | Investigating |
| PM | **No output files found.** No Cardiff Bay directory exists in `bin/World/`. Generation produced nothing | Critical | Root cause found |
| PM | **Root cause: dual code paths.** Editor has two completely separate world generation paths. Fixes went to Path A (S-57 chart via `WorldGenerator`), but Cardiff Bay hits Path B (no-chart fallback in `EditorApp.cpp:2181-2511`) because no free UK S-57 charts exist | Critical | Documented |
| PM | Path B has its own inline barrier flood-fill (~300 lines) that does NOT use `HeightmapGenerator`. It also has the unfixed `< 0.0f` IEEE 754 bug | High | Unfixed |
| PM | Overpass API returns 504 timeout for Cardiff Bay barrier query (`waterway=dam`). Timeout silently swallowed by `catch(...)` at `EditorApp.cpp:2371`. Barriers empty, flood-fill skipped | High | Unfixed |
| PM | Cardiff Bay Barrage confirmed present in OSM as `waterway=dam` (way 1287700755, 63-node closed polygon). Data exists, query just times out | Info | Verified |
| PM | 8 new barrier tests added (closed polygon, headland+barrier, L-shape harbour walls, -0.0f edge case, multi-barrier OSM response, malformed JSON). Total: 204 tests, 10905 assertions, 0 failures | -- | Done |

**Action items created:** Task 0.3a (refactor fallback to use `HeightmapGenerator`), Task 0.3b (Overpass timeout retry + user feedback).

| PM | **Fixed:** IEEE 754 `-0.0f` bug in fallback path -- changed 5 comparisons from `< 0.0f` to `<= 0.0f` in `EditorApp.cpp` flood-fill (lines 2441-2483) | High | Fixed |
| PM | **Fixed:** Overpass barrier query now retries with fallback server (`overpass.kumi.systems`), increased timeout (90s/120s), logs errors to stderr, shows warning to user when barriers unavailable | High | Fixed |
| PM | Editor rebuilt (x86), all 204 unit tests passing | -- | Done |

**Status:** Tasks 0.3a (full refactor) deferred. Critical bugs in fallback path fixed directly. Ready for Cardiff Bay retest.

### 2026-02-20: Architecture review and barrier regression fix (Tasks 0.3c-0.3f)

**Context:** User reported full regression -- no barrage, no bay, no harbour, Flat Holm island missing. Architecture review identified 5 bugs.

| Time | Finding | Severity | Status |
| --- | --- | --- | --- |
| -- | **Architecture review** found 5 bugs in barrier data pipeline (3 high, 2 medium) | -- | All code bugs fixed |
| -- | **Root cause of regression:** 8-pass 5x5 box blur ran AFTER barrier flood-fill, eroding thin barrier lines (2.0m) against deep water (-20.0m) back below sea level. Fixed: blur now runs BEFORE flood-fill (task 0.3c) | High | Fixed |
| -- | **Closed polygon snapping bug:** Cardiff Bay Barrage is closed polygon (first == last vertex). Endpoint snapping checked only front()/back() which are identical. Only one end connected to land, BFS entered around the other. Fixed: snapping checks ALL vertices (task 0.3f) | High | Fixed |
| -- | `OSMBuildingReader.query()` does not clear `barrierLines` on re-query. `saveCache()`/`loadCache()` do not serialize barrier lines. Fixed both (task 0.3d) | Medium | Fixed |
| -- | Separate Overpass barrier query eliminated. Barrier data extracted from building query response via `getBarrierLines()`. Reduced Overpass requests from 3 to 2 (task 0.3e) | Medium | Fixed |
| -- | `generateStatus` (std::string) data race between UI and background thread. `std::thread(...).detach()` with captured `this` -- use-after-free on shutdown | High | Documented (task 0.9) |
| -- | **Flat Holm island** (620m, 32m elevation, active lighthouse) missing from generated worlds. Root cause: Natural Earth 10m coastline data does not include small islands. Not a regression -- pre-existing NE data limitation (Issue 8) | Medium | Documented (task 0.8) |
| -- | All 149 tests passing (10610 assertions). Editor rebuilt (x86) | -- | Done |

### 2026-02-20: OSM land polygons and elevation data research (Tasks 0.8, 0.10)

**Context:** User requested replacement of Natural Earth with OSM land polygons, then asked about real elevation data to eliminate the blur.

| Time | Finding | Severity | Status |
| --- | --- | --- | --- |
| -- | **OSM land polygons implemented** (task 0.8). Three-tier system: OSM shapefile (GDAL rasterization) > NE + island supplement > NE only. `OSMLandPolygons` class created. 1.3GB shapefile downloaded to `bin/Data/land-polygons-split-4326/` | -- | Done |
| -- | **Island supplement added.** `place=island\|islet` ways extracted from Overpass building query. Scanline rasterized onto heightmap. Cache serialization added (ISLANDS section) | -- | Done |
| -- | **Land clamping after blur.** Pre-blur land mask saved, confirmed-land pixels clamped to min 1.0f post-blur. Prevents blur from drowning confirmed land | -- | Done |
| -- | **Barrier snap radius increased** from 20 to 40 pixels for better OSM coastline geometry matching | -- | Done |
| -- | **Identified root cause of blur dependency**: no real elevation data in no-chart path. All land flat 2.0m, all water flat -20.0m. Blur exists only to create gradual slopes at coastlines (Issue 10) | High | Documented |
| -- | **Elevation data research.** Surveyed 8+ global DEM sources. AWS Terrain Tiles (Terrarium PNG) recommended: same z/x/y pattern as ESRI satellite tiles, free S3, no auth, land+bathy, ~20-80KB tiles | -- | Documented (task 0.10) |
| -- | **Discovered unused infrastructure**: `GEBCOReader` and `HeightmapGenerator.loadDEMTiles()` are fully implemented but never wired into the no-chart generation path | Medium | Documented |

### 2026-02-19: Barrier flood-fill extracted to standalone utility (Task 0.3a)

**Context:** Task 0.3a -- eliminate code duplication between S-57 and no-chart barrier handling.

| Time | Finding | Severity | Status |
| --- | --- | --- | --- |
| -- | Created `BarrierFloodFill.hpp/cpp` as a GDAL-independent standalone utility. Shared algorithm: Bresenham rasterize, 1px dilate, edge-seeded BFS, reclaim enclosed water | -- | Done |
| -- | `HeightmapGenerator::applyBarrierFloodFill()` now delegates to standalone function (flatten 2D grid, call, unflatten) | -- | Done |
| -- | `EditorApp.cpp` fallback path: replaced ~110 lines of inline barrier code with single `BarrierFloodFill::apply()` call | Duplication | Fixed |
| -- | **Found latent bug:** barrier boundary pixels were getting `reclaimHeight` (1.0m) instead of `barrierHeight` (2.0m) due to ordering issue in reclaim loop. Both the original `HeightmapGenerator` and `EditorApp` copies had this bug | Medium | Fixed |
| -- | Added 8 standalone barrier tests (box, no-barrier, non-enclosing, -0.0f, land preservation, null grid, custom heights, headland+barrier) | -- | Done |
| -- | All 212 tests passing (with GDAL), 147 without GDAL. Editor rebuilt (x86) | -- | Done |

### 2025-02-14 to 2025-02-17: Initial world generation development

| Date | Change | Files |
| --- | --- | --- |
| Feb 14 | World generation pipeline operational. S-57 chart reading, IDW heightmap, satellite texture working. Test worlds: River, SimpleEstuary, SimpleEstuaryMultiResolution | `WorldGenerator.cpp`, `HeightmapGenerator.cpp`, `SatelliteTexture.cpp` |
| Feb 16 | Portsmouth Harbour world generated | `bin/World/PortsmouthHarbour/` |
| Feb 17 | Santa Catalina and Swinomish Channel worlds generated. OSM water polygons, LNDARE water holes, coastline accuracy improvements | `OSMWaterReader.cpp`, `HeightmapGenerator.cpp` |
