# Photorealistic World Generation Plan

Auto-generate terrain, buildings, and land features from chart/OSM data.

## Current State

- `ChartReader` extracts buoys, lights, landmarks, depths, coastlines from S-57 charts
- `WorldGenerator` produces terrain.ini, height.png, texture.png, buoy/light/landobject.ini
- `SatelliteTexture` composites ESRI tiles into a single terrain texture
- `TileDownloader` fetches tiles async with disk+memory cache
- `IrrlichtModelConverter` bridges .3ds/.x models to WE mesh entities
- Buildings are pre-made .3ds/.x models (Castle, House, Church, etc.) -- no procedural generation
- Tile download is single-threaded (~10 tiles/sec)

## Architecture Overview

```
S-57 chart / OSM .pbf
       |
  ChartReader / OSMReader
       |
  WorldGenerator
       |
  +---------+-----------+-----------+
  |         |           |           |
height.png texture.png buoy.ini  buildings/
(terrain)  (satellite)  (lights)  (procedural meshes)
```

---

## Phase 1: Better Static Models

**Goal:** Replace flat-looking .3ds buildings with PBR-textured .obj models. Zero code changes.

### Task 1.1: Source PBR building textures
- Download CC0 facade textures from ambientCG.com (search "facade", "brick", "plaster")
- Need: albedo (.png), normal (_Normal.png), roughness (_Roughness.png) per facade type
- Target: 5-8 facade types (brick residential, plaster, concrete, stone church, castle wall, industrial)
- Save to `bin/Models/LandObject/{type}/textures/`

### Task 1.2: Convert key .3ds models to .obj with PBR
- Pick the 5 most-used buildings: House, BlueHouse, CreamHouse, Church, Castle
- Open each in Blender, re-UV-map to the PBR facade textures, export as .obj
- Include .mtl file referencing albedo + normal + roughness maps
- Test: `loadModelOrPlaceholder()` already handles .obj via WickedModelImporter

### Task 1.3: Add PBR map loading to WickedModelImporter
- Currently loads BASECOLORMAP only. Add NORMALMAP and SURFACEMAP (roughness/metalness)
- In `createWEMeshFromConverted()`: check for `_Normal.png` and `_Roughness.png` alongside base texture
- Convention: `wall.png` -> `wall_Normal.png`, `wall_Roughness.png`

---

## Phase 2: OSM Building Footprint Parsing

**Goal:** Read building polygons from OSM data so we know where to place procedural buildings.

### Task 2.1: Add earcut.hpp to the project
- Download from github.com/mapbox/earcut.hpp (single header, ISC license)
- Place in `src/libs/earcut/earcut.hpp`
- Add to include path in .vcxproj

### Task 2.2: Add libosmium dependency (optional, or use Overpass API)
- **Option A (simpler):** Use existing `OpenSeaMapSource` Overpass API to query building footprints
  - Query: `[out:json];way["building"](bbox);out geom;`
  - Parse JSON response with nlohmann/json (already in project)
- **Option B (faster, offline):** Add libosmium for .pbf parsing
  - Header-only, BSL-1.0 license
  - Place in `src/libs/libosmium/`

### Task 2.3: Create OSMBuildingReader class
- File: `src/editor/OSMBuildingReader.hpp/cpp`
- Input: bounding box (lat/lon)
- Output: vector of `BuildingFootprint`:
  ```cpp
  struct BuildingFootprint {
      std::vector<std::pair<double,double>> outline; // lat/lon polygon
      float height;       // metres (from building:height or building:levels * 3)
      std::string type;   // residential/commercial/industrial/church/...
      std::string name;   // building name if available
  };
  ```
- Method: query Overpass API for `way["building"]` in bbox, parse geometry + tags

### Task 2.4: Display building footprints in the scenario editor
- In `EditorApp::renderMapPanel()`, draw footprint polygons on the map
- Simple filled rectangles at first, using the map projection already in place
- Toggle with a keyboard shortcut (B for buildings)

---

## Phase 3: Procedural Building Mesh Generation

**Goal:** Extrude building footprints into 3D meshes with facade textures.

### Task 3.1: Create BuildingGenerator class
- File: `src/BuildingGenerator.hpp/cpp`
- Input: `BuildingFootprint` + coordinate converter (longToX/latToZ)
- Output: WE `MeshComponent` data (vertex_positions, normals, uvs, indices)
- Algorithm:
  1. Convert footprint lat/lon to world X/Z coordinates
  2. Triangulate floor polygon with earcut
  3. Extrude walls: for each edge, create 2 triangles (quad) from ground to height
  4. UV-map walls: U = distance along edge, V = 0 at ground, 1 at top
  5. Add roof (flat initially: copy floor triangles at height Y)

### Task 3.2: Create a facade texture atlas
- Single 2048x2048 atlas with ~16 facade strips arranged in rows
- Each row = one facade type at a specific height (e.g., 3m per storey)
- UV coordinates reference atlas regions based on building type + height
- Include PBR maps: atlas_albedo.png, atlas_normal.png, atlas_roughness.png
- Store in `bin/Models/BuildingAtlas/`

### Task 3.3: Wire BuildingGenerator into WorldGenerator
- After terrain and buoys are generated, query OSM buildings for the world bbox
- For each building footprint: generate mesh, write as .obj to world output dir
- Add entries to landobject.ini with position/rotation

### Task 3.4: Batch buildings for performance
- Group buildings into tiles (e.g., 500m x 500m grid)
- Merge all building meshes in a tile into a single MeshComponent
- Share the facade atlas material across all buildings in a tile
- Target: <100 draw calls for a typical harbour area

---

## Phase 4: Enhanced Terrain Textures

**Goal:** Better ground textures using satellite imagery + procedural detail.

### Task 4.1: Multi-zoom satellite tile streaming
- Current: single zoom level, all tiles at once
- New: download low-zoom tiles first (fast preview), then high-zoom for close areas
- Modify `SatelliteTexture::generate()` to accept a priority region (near own ship)

### Task 4.2: Terrain PBR materials
- Generate normal map from heightmap gradient (Sobel filter on height.png)
- Assign roughness by land-use type:
  - Water: 0.05 (glossy)
  - Urban: 0.6 (moderate)
  - Grass/rural: 0.8 (rough)
  - Sand/beach: 0.7
- Splatmap from ChartReader land-use zones (LNDARE, BUAARE, etc.)

### Task 4.3: Detail texture blending
- At close range (<500m), satellite imagery looks blurry
- Blend in tileable detail textures (grass, concrete, sand) based on splatmap
- WE supports material layer blending per pixel

---

## Phase 5: Tile Pipeline Performance

**Goal:** Fix the slow tile download bottleneck.

### Task 5.1: Multi-threaded tile downloads
- Replace single worker thread with thread pool (4 workers)
- Each worker respects rate limiting per domain (100ms between same-domain requests)
- Pool shared across all TileDownloader instances

### Task 5.2: HTTP connection pooling
- Reuse WinHTTP session handles across requests to same domain
- Avoids TLS handshake overhead per tile
- Store HINTERNET handles per domain in TileDownloader

### Task 5.3: Progressive world generation
- Show low-res satellite preview immediately (zoom 10-12)
- Fill in high-res tiles in background (zoom 15-17)
- Allow user to start editing while tiles load

---

## File Map

| File | Purpose |
|---|---|
| `src/libs/earcut/earcut.hpp` | Polygon triangulation (Phase 2) |
| `src/editor/OSMBuildingReader.hpp/cpp` | Parse OSM building footprints (Phase 2) |
| `src/BuildingGenerator.hpp/cpp` | Extrude footprints to 3D meshes (Phase 3) |
| `bin/Models/BuildingAtlas/` | Facade texture atlas (Phase 3) |
| `src/editor/SatelliteTexture.cpp` | Enhanced terrain textures (Phase 4) |
| `src/editor/TileDownloader.cpp` | Performance fixes (Phase 5) |

## Dependencies

| Library | License | Purpose | Phase |
|---|---|---|---|
| earcut.hpp | ISC | Polygon triangulation | 2 |
| nlohmann/json | MIT | Already in project, parse Overpass responses | 2 |
| stb_image | Public domain | Already in project, decode PBR textures | 1 |
| ambientCG textures | CC0 | Building facade textures | 1, 3 |
