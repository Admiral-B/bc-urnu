# Ketchup Plan: Phases 4-8

## TODO

### Phase 9: Ocean Shader Realism

50m FFT tiling is the biggest visual weakness. Three improvements, in order:

- [x] Burst 35: WE spectrumCallback + randomSeed extension -- added to OceanParameters (wiOcean.h), initHeightMap() (wiOcean.cpp). Rebuilt WE lib. [depends: none]
- [x] Burst 36: OceanCB layout extension -- added normalOverlayIndex, cascade0/2 grad indices and weights to ShaderInterop_Ocean.h + OceanParameters. Deleted all .cso/.wishadermeta. Rebuilt WE lib. [depends: 35]
- [x] Burst 37: Normal map overlay -- 512x512 FBM noise normal texture, bindless in oceanSurfacePS.hlsl, 500m period with slow animation [depends: 36]
- [x] Burst 38: Multi-cascade blending -- REVERTED. Aux cascades share 50m patch_length, creating checkerboard artifacts. JONSWAP reverted: peak at typical winds outside FFT range. Phillips retained. [depends: 37]
- [x] Burst 39: patch_length 50->250m -- eliminates geometric tiling at root cause. K_min drops from 0.126 to 0.025 rad/m, Phillips K^-6 gives ~15000x more energy per mode. BEAUFORT_AMPLITUDE reduced from {2..1700} to {2..50}. choppy_scale 3x compensates GridLen reduction. Normal Y scale hardcoded to 0.2 (decoupled from xOceanTexelLength). Multi-scale VS/PS displacement hacks removed (single clean FFT sample). [depends: 38]
- [ ] Burst 40: Whitecap foam -- Jacobian fold (gradient.a) values are 1-10+ at current choppy_scale, too broad for simple thresholding. Needs either: (a) reduce choppy_scale and boost fold sensitivity, (b) screen-space foam approach, or (c) separate foam compute pass with proper per-Beaufort thresholds. [depends: 39]

### Phase 8: GLB Ship Model Integration

- [x] Burst 29: `tools/glb_inspect.py` parses GLB, extracts geometry bounds and PBR texture inventory [depends: none]
- [x] Burst 30: Script generates starter boat.ini with estimated scale, camera views, physics, nav lights [depends: 29]
- [x] Burst 31: Four GLB othership models added -- CargoShip (170m Handysize), HMS_Clyde (81.5m OPV), USS_Perry (FFG-7 135.6m), USS_Zumwalt (DDG-1000 186m) with hand-tuned boat.ini, nav lights, angle corrections [depends: 30]
- [x] Burst 32: Ownship directory fallback -- WickedMain.cpp ownship loader now checks Models/Othership/ when model not found in Models/Ownship/ (mirrors existing othership fallback). boat.ini files updated with Views (bridge/wing/overhead), maxSpeedAhead, basic physics for all 4 GLB ships [depends: 31]
- [ ] Burst 33: In-game tuning pass -- verify bridge camera positions, model waterline alignment, and physics feel for all 4 ships as ownship [depends: 32]
- [ ] Burst 34: Ownship-specific model enhancements -- MakeTransparent for bridge windows, radar.ini, detailed bridge walk bounds [depends: 33]

## DONE

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
