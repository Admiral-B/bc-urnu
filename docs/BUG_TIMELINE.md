# Bug Timeline: World Generation & Rendering

Tracks bugs found during testing, root causes, and fixes.

---

## 2026-02-25 (Round 6): Terrain Realism, Island Fix, Dock Waterfront

### FIX: Terrain undulation destroying narrow coastal features

- **Symptom**: Deep parallel ridges/grooves on breakwaters, spits, narrow land strips
- **Root cause**: Undulation pass had 1m minimum amplitude applied to 0.3m beach ramp features
- **Fix**: Skip land <5m elevation entirely, quadratic amplitude ramp (`above5^2 * 0.008`), max 4m. No minimum floor.
- **File**: `src/HeightmapGenerator.cpp` (Pass 4b)

### FIX: Lighthouse island bowl/crater shape

- **Symptom**: Synthetic lighthouse islands appeared as bowls instead of domes
- **Root cause**: DEM override `if (elev > 1.0f)` replaced synthetic 5m dome with lower DEM values (1-2m) since DEM lacks data for tiny islands
- **Fix**: Changed to `if (elev > heightGrid[i])` -- only override when DEM is HIGHER than existing height. Also removed islandMask skip from beach ramp so islands get natural beach transitions.
- **File**: `src/editor/EditorApp.cpp` (Step 3 DEM merge + Step 3.5 beach ramp)

### FIX: Dune bumps creating parallel ridges

- **Symptom**: Visible banding at fixed distances from coast
- **Root cause**: `sin(d * 0.9)` where d = distance from coast created periodic ridges
- **Fix**: Removed dune bumps entirely.
- **File**: `src/HeightmapGenerator.cpp`

### FIX: Dock/quay walls appearing as organic sandy slopes

- **Symptom**: Roath Dock walls looked like natural beach instead of straight concrete edges
- **Root cause**: Beach ramp + sand texture + coastal noise applied uniformly to all coastline including man-made waterfronts
- **Fix**: New dock waterfront mask system. OSM dock/marina water dilated 3px, intersected with land -> `dockEdgeMask`. These pixels skip beach ramp, get sharp 1px transition, concrete texture (Waterfront type 19), roughness 0.50.
- **Files**: `EditorApp.cpp`, `TerrainTextureBlender.cpp`, `OSMLandUseReader.hpp`

### FIX: Procedural terrain texture too green / overpowering satellite

- **Symptom**: Terrain looked unnaturally green, satellite imagery barely visible
- **Fix**: Muted grass colours (r:55->70, g:110->95), reduced blend alpha (unclassified 0.55->0.30, land-use 0.65->0.45), wider sand transition (30m fixed -> 40-80m noise-modulated)
- **File**: `src/editor/TerrainTextureBlender.cpp`

---

## 2026-02-23 (Round 5): glTF Importer, Beach Ramps, Structure Fixes

### FIX: glTF/GLB model importer ported from WickedEngine Editor

- **Symptom**: `wi::scene::LoadModel()` only loads `.wiscene` archives, GLB files triggered archive version error
- **Fix**: Ported `ImportModel_GLTF()` (~2060 lines + tinygltf header) from WE Editor. Handles meshes, materials (PBR metallic-roughness, specular-glossiness, extensions), transforms, animations, lights.
- **Excluded**: VRM/Mixamo extensions (stubbed), EXT_lights_image_based (#if 0'd -- needs dds.h/stb_image)
- **File**: `src/graphics/wicked/ModelImporter_GLTF.cpp`, `src/libs/tinygltf/tiny_gltf.h`

### FIX: Beach/coastal terrain ramp replaces cliff edges

- **Symptom**: Sharp cliff at land/water boundary (0.5m land next to -0.5m water)
- **Root cause**: Binary land/water classification with re-clamping created vertical transitions
- **Fix**: Two-pass chamfer distance transform from coastline boundary, then smoothstep falloff over 6 pixels (~60m). Land side ramps from 0.3m to DEM height; water side ramps from -0.2m to charted depth. Island mask pixels excluded.
- **File**: `src/editor/EditorApp.cpp` (new pass after coastal smoothing)

### FIX: Pier structures elevated to deck height

- **Symptom**: Structures on Penarth pier appeared to float at sea level (0m)
- **Fix**: Pier/jetty structures clamped to 1.0m (approximate deck height) instead of 0.0m. Other structures (breakwaters, dams) still at sea level.
- **File**: `src/editor/EditorApp.cpp`

### FIX: Overpass API HTML error response detection

- **Symptom**: "JSON parse error... last read: '<'" when Overpass returns rate-limit HTML page
- **Fix**: All 4 Overpass callers now check first non-whitespace char is `{` or `[` before accepting response. Non-JSON triggers retry on fallback server.
- **File**: `OpenSeaMapSource.cpp`, `OSMBuildingReader.cpp`, `OSMWaterReader.cpp`, `OSMLandUseReader.cpp`

---

## 2026-02-23 (Round 4): Lighthouse & Cardinal Buoy Fixes

### FIX: Procedural lighthouse geometry replaces broken .x model

- **Symptom**: Lighthouses appeared as cream boxes (Round 3 placeholder) or red disks (earlier)
- **Root cause**: Irrlicht .x loader merges all 8 frame meshes into 1 buffer when frames lack MeshMaterialList. Only Tower and Disc_ had explicit materials; the other 5 meshes (including the helipad) had none, so everything merged with the helipad.bmp texture.
- **Fix**: Created `createProceduralLighthouse()` generating a multi-section cylinder: cream tapered tower, dark grey gallery platform, dark blue-grey lantern room, red dome. Height parameterized from OSM `seamark:landmark:height` / `seamark:light:height` tag (default 15m). Cached by height.
- **File**: `src/WickedMain.cpp` (createProceduralLighthouse), `src/editor/OpenSeaMapSource.cpp` (HeightAbove field)

### FIX: Cardinal buoy colours now use procedural banded cylinders

- **Symptom**: Cardinal buoys showed incorrect colour bands despite correct IALA strings in buoy.ini
- **Root cause**: Pillar buoy model has uneven vertex distribution (wide base concentrates most triangles in bottom band). Per-triangle Y-banding assigned most visible geometry to one colour.
- **Fix**: Multi-band buoys (>1 colour) now use procedural 12-segment cylinder with guaranteed equal-height bands. Single-colour buoys still use model geometry. Colour matching made case-insensitive.
- **IALA-A cardinal marks verified**: N=black/yellow, E=black/yellow/black, S=yellow/black, W=yellow/black/yellow (top-to-bottom)
- **File**: `src/WickedMain.cpp` (createBandedBuoyCylinder, loadBuoyWithColour)

### FIX: Harbour wall barriers now queried from OSM

- **Symptom**: Land appears between harbour wall arms (e.g. harbour entrance blocked by solid land)
- **Root cause**: `man_made=harbour_wall` not included in Overpass query or barrier line collection
- **Fix**: Added `harbour_wall` to Overpass query, structure type detection, barrier line collection, and height estimation (5m default).
- **File**: `src/editor/OSMBuildingReader.cpp`

### KNOWN: Queen Alexandra Dock lock structures missing

- **Symptom**: Lock gates and dock walls not visible in Queen Alexandra Dock
- **Root cause**: Lock gates (`waterway=lock_gate`) are OSM nodes (points), not ways (polygons). Can't be captured as building footprints. Dock water is queried via `waterway=dock` but physical structures need quay wall rendering.
- **Future fix**: Add procedural lock gate geometry at node positions, or import 3D lock gate model.

### FIX: Lighthouse uses procedural geometry (GLB not supported)

- **Symptom**: GLB model triggered WE archive version error dialog. `wi::scene::LoadModel()` only loads `.wiscene` archives, not glTF/GLB.
- **Fix**: Removed GLB loading attempt. Lighthouses always use `createProceduralLighthouse()` (cream tower, grey gallery, blue-grey lantern, red dome). Height from OSM `HeightAbove` tag, default 15m.
- **Lesson**: WickedEngine's `LoadModel` is NOT a glTF importer. Need a proper glTF import path (tinygltf) before GLB models can be used.
- **File**: `src/WickedMain.cpp`

### FIX: Harbour arms / area-polygon breakwaters no longer generate terrain fill

- **Symptom**: Curved harbour arms had solid land underneath instead of water
- **Root cause (1)**: `harbour_wall` was added to barrier lines. Fixed by removing from barrier collection.
- **Root cause (2)**: Cardiff Bay harbour arms are tagged `man_made=breakwater` with `area=yes` (closed polygons, not linear ways). The barrier line collection didn't check `isClosed`, so area-polygon breakwaters were added as barriers too, generating terrain fill.
- **Fix**: Barrier line collection now requires `!isClosed`. Only linear (open-way) dams/breakwaters generate terrain. Closed-polygon breakwaters are extruded as concrete structures without terrain fill.
- **Key distinction**: Linear dam/breakwater = barrier (terrain fill). Area breakwater = structure (no fill).
- **File**: `src/editor/OSMBuildingReader.cpp`

### FIX: Cardinal buoy topmark cones (IALA standard)

- **Symptom**: Cardinal buoys had correct colour bands but no topmark cones to distinguish N/S/E/W at a glance
- **Fix**: Added `inferCardinalTopmark()` to detect cardinal type from colour pattern, `addConeToMesh()` for cone geometry, and extended `createBandedBuoyCylinder()` with topmark support. IALA standard: N=both up, S=both down, E=base-to-base, W=point-to-point. All cones black. Staff extends above buoy body.
- **File**: `src/WickedMain.cpp` (inferCardinalTopmark, addConeToMesh, createBandedBuoyCylinder)

---

## 2026-02-23 (Round 3): Regression Recovery

### REGRESSION: Cardiff Bay turned to land, buildings disappeared
- **Symptom**: Entire bay area became green land, buildings inside gone
- **Root cause**: Two compounding issues: (1) Building skip threshold changed from -5m to -2m removed all buildings in the enclosed bay (terrain below -2m). (2) Overpass API rate limiting caused water query failure, so bay area wasn't subtracted from land mask.
- **Fix**: Reverted building threshold to -5m. Reverted Y clamping. Added 3-second stagger between all Overpass API queries.
- **Lesson**: Never change terrain/building thresholds without understanding the specific area's elevation profile. Cardiff Bay inner basin is at -3 to -20m.

### REGRESSION: Satellite texture 2x change
- **Symptom**: Potential rendering issues from mismatched texture/heightmap sizes
- **Fix**: Reverted satellite texture 2x. Texture resolution is a user-controlled parameter (resolution selector in editor).

### BUG: Lighthouse model shows as red disk (root cause found)
- **Symptom**: Red disk at every lighthouse position regardless of ScaleFactor
- **Root cause**: Eddystone model loads as 1 submesh with `helipad.bmp` as the only applied texture. The Irrlicht converter maps helipad texture across the entire 145-vertex mesh. Changing ScaleFactor only resizes the red disk.
- **Fix**: Override lighthouse rendering in WickedMain to use a 30m tall cream-colored placeholder box. The Eddystone .x model needs proper texture mapping in a future pass.
- **File**: `src/WickedMain.cpp` (land object loading)

### BUG: Synthetic islands still appearing on piers
- **Symptom**: 150m proximity check not reaching coast from pier end
- **Root cause**: Penarth pier is ~200m long. 150m radius from pier end doesn't reach the coastline.
- **Fix**: Increased proximity radius from 150m to 300m.

### FIX: Overpass API rate limiting causing query failures
- **Symptom**: "Retrying with fallback Overpass server..." followed by failed water/lock queries
- **Root cause**: 4 Overpass queries in rapid succession hit the ~2 req/min rate limit. Fallback server also rate-limits under load.
- **Fix**: Added 3-second `sleep_for()` delays before buildings, water, and land-use queries. Total generation time increases ~9 seconds but queries succeed reliably.
- **File**: `src/editor/EditorApp.cpp`

### CONFIRMED: Per-triangle buoy colour banding works correctly
- **Evidence**: Simulation log shows pillar buoys loading with 3 bands (188 verts, 262 tris). Can/conical buoys also loading with correct band counts.
- **Note**: If buoys appear missing after regeneration, check if seamark query succeeded (rate limit may have caused failure in the specific generation run).

---

## 2026-02-23 (Round 2): Post-Testing Fixes

### BUG: Cardinal buoy colours still wrong after band ordering fix
- **Symptom**: Multi-band patterns (yellow;black;yellow) not showing correctly on buoys
- **Root cause**: Per-submesh colour assignment doesn't work -- can/conical models have only 1 submesh, so ALL triangles get one colour. Even pillar (6 submeshes) is too coarse for 3-band patterns.
- **Fix**: Rewrote `loadBuoyWithColour()` to use per-TRIANGLE colour banding. Merges all submesh geometry into one vertex pool, assigns each triangle to a colour band based on its centroid Y position, then creates one WE subset per band.
- **File**: `src/WickedMain.cpp` (loadBuoyWithColour)

### BUG: Giant red disks on lighthouse positions
- **Symptom**: Oversized red circular objects on Monkstone, Penarth pier, other lighthouse locations
- **Root cause**: Lighthouse ScaleFactor=5.0 made the Eddystone model's helipad platform enormous (multiple hundred metres across). The helipad texture (helipad.bmp) is red.
- **Fix**: Reduced ScaleFactor from 5.0 to 2.0 in `bin/Models/LandObject/Lighthouse/object.ini`

### BUG: Synthetic islands still appearing on piers (Penarth)
- **Symptom**: Penarth pier lighthouse gets a 50m radius island despite cat-17 fix
- **Root cause**: Penarth pier lighthouse IS category 99 (actual lighthouse), so the cat-17-only filter didn't help. Any lighthouse within 150m of existing land (piers, breakwaters, coastal structures) shouldn't get an island.
- **Fix**: Added proximity-to-land check: scan ~150m radius around lighthouse position and skip island creation if any existing land pixels found.
- **File**: `src/editor/EditorApp.cpp` (Step 1.4)

### BUG: Buildings floating in enclosed bay areas
- **Symptom**: Buildings near Cardiff Bay waterfront appear to float above water
- **Root cause**: Buildings on enclosed bay edges sample terrain at -2 to -5m (below sea level in the inner basin). They pass the -5m skip threshold but get placed below the water surface.
- **Fix**: Tightened skip threshold from -5m to -2m. Non-structure buildings with groundY < 0 clamped to sea level (0m). This matches reality -- real buildings sit at or above sea level.
- **File**: `src/editor/EditorApp.cpp` (building generation)

### BUG: Land texture too low resolution
- **Symptom**: Satellite imagery appears blurry/pixelated at close range
- **Root cause**: Satellite texture was generated at same size as heightmap (e.g. 2049x2049 for ~12km = 6m/pixel). Terrain geometry doesn't need that many pixels, but textures do.
- **Fix**: Satellite texture now generated at 2x heightmap resolution (capped at 4097). For 2049 heightmap, texture is 4097x4097 = ~3m/pixel.
- **File**: `src/editor/EditorApp.cpp` (satellite texture download)

### NOTE: Red objects at lighthouse positions = light model artifacts
- **Symptom**: Red circular objects visible near lighthouses
- **Explanation**: These were the Eddystone lighthouse helipad at ScaleFactor=5.0. Fixed by reducing to 2.0.

### NOTE: Building textures appearing white
- **Explanation**: Older world generations may have incomplete MTL files (missing texture references). Current code generates proper MTL with wall/roof textures, normal maps, roughness maps. Regenerating the world with current code should produce textured buildings.

### KNOWN: Lock structures intermittently missing
- **Symptom**: Lock channels through Cardiff Bay barrage sometimes absent after world generation
- **Root cause**: Overpass API rate limiting can cause water query failure. Lock polygons captured in Step 1.5 and re-cut in Step 4.1 -- empty if query fails.
- **Workaround**: Regenerate world. Fallback server usually succeeds.

### KNOWN: Land between harbour wall arms
- **Symptom**: Solid land appears between pier/harbour wall arms instead of water
- **Root cause**: `man_made=harbour_wall` not queried by OSMBuildingReader. Only `dam` and `breakwater` are barrier lines.
- **Future fix**: Add `harbour_wall`, `seawall` to Overpass query and barrier collection.

### KNOWN: Overpass API rate limiting
- **Symptom**: "Retrying with fallback Overpass server..."
- **Root cause**: Primary server enforces ~2 req/min. World generation makes 4+ queries.
- **Mitigation**: Automatic fallback to `overpass.kumi.systems`.

---

## 2026-02-23 (Round 1): Initial Buoy Colour Implementation

### BUG: Cardinal buoy colour bands reversed
- **Symptom**: North cardinal showed yellow on top / black on bottom
- **Root cause**: IALA strings are top-to-bottom but code mapped bottom-to-top
- **Fix**: Inverted Y normalization. Superseded by Round 2 per-triangle rewrite.

### BUG: Spurious islands under towers on piers
- **Symptom**: Random land mounds at pier ends
- **Root cause**: Collected both towers (cat 17) AND lighthouses (cat 99) for synthetic islands
- **Fix**: Changed to cat 99 only. Superseded by Round 2 proximity check.

### BUG: Lighthouses invisible at ScaleFactor=1.0
- **Symptom**: Lighthouses not visible in simulation
- **Root cause**: Eddystone model too small at 1.0 vs houses (2.4) and churches (4.0)
- **Fix**: Increased to 5.0, then reduced to 2.0 in Round 2 (helipad was too large at 5.0).

---

## Earlier Issues (pre-buoy-colours)

### FIX: Monkstone Island missing from terrain
- **Fix**: Synthetic lighthouse islands with 50m radius, quadratic height falloff, islandMask protection.

### FIX: Buildings floating above terrain
- **Fix**: Use actual terrain height, not clamped to 0. Refined in Round 2 with -2m threshold.

### FIX: Lighthouses only extracted from OSM nodes, not ways
- **Fix**: Process both nodes and ways for lighthouse extraction.

### FIX: `man_made=lighthouse` landmarks ignored when `seamark:type` also present
- **Fix**: Create landmark for `man_made=lighthouse` regardless of seamark tags.
