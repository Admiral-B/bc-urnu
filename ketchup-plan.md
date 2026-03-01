# Ketchup Plan: Phases 4-6 (Terrain Textures, Tile Performance, GBA Heights)

## TODO

(none -- all bursts complete)

## DONE

### Bottle: TerrainNormalMap

- [x] Burst 1: Sobel normal map from heightmap produces correct tangent-space normals for flat terrain [depends: none]
- [x] Burst 2: Sobel normal map handles steep slopes and edge pixels correctly [depends: 1]
- [x] Burst 3: Normal map output is correct dimensions and RGB encoding (128,128,255 = flat) [depends: 2]

### Bottle: TerrainRoughnessMap

- [x] Burst 4: Roughness classification returns correct value per land-use type [depends: none]
- [x] Burst 5: Roughness map generates correct dimensions and single-channel output [depends: 4]

### Bottle: DetailTextureBlending

- [x] Burst 6: Slope-based weight calculation returns rock for steep, grass for flat [depends: none]
- [x] Burst 7: Water proximity weight calculation returns sand near water, dirt in transition [depends: none]
- [x] Burst 8: Elevation-based weight shifts grass to rock at high altitude [depends: none]
- [x] Burst 9: TerrainTextureBlender::blend integrates all weight sources with noise modulation [depends: 6, 7, 8]
- [x] Burst 10: Close-range detail textures blend into satellite imagery below 500m threshold [depends: 9]

### Bottle: TileThreadPool

- [x] Burst 11: Thread pool distributes work across N workers and completes all queued items [depends: none]
- [x] Burst 12: Thread pool respects per-domain rate limiting (100ms between same-domain requests) [depends: 11]
- [x] Burst 13: Thread pool graceful shutdown joins all workers without deadlock [depends: 11]

### Bottle: WinHTTPConnectionPool

- [x] Burst 14: Connection pool creates one HINTERNET session per TileDownloader lifetime [depends: none]
- [x] Burst 15: Connection pool reuses HINTERNET connect handles per domain [depends: 14]
- [x] Burst 16: Wire connection pool into TileDownloader::httpDownload replacing per-request session creation [depends: 15]

### Bottle: TileDownloaderMultiThread

- [x] Burst 17: TileDownloader uses thread pool (4 workers) instead of single worker [depends: 11, 12, 13, 16]
- [x] Burst 18: SatelliteTexture and ElevationTile work correctly with multi-threaded TileDownloader [depends: 17]

### Bottle: ProgressiveWorldGen

- [x] Burst 19: SatelliteTexture supports two-pass generation (low-zoom fast, high-zoom detail) [depends: 17]
- [x] Burst 20: Editor shows low-res preview immediately while high-res tiles load in background [depends: 19]

### Bottle: GBATileDownloader

- [x] Burst 21: GBA tile name computed correctly from lat/lon bounding box [depends: none]
- [x] Burst 22: GBA GeoJSON parser extracts building polygons with height and variance [depends: none]
- [x] Burst 23: GBA spatial index (grid-based) finds nearest polygon for a given centroid [depends: 22]
- [x] Burst 24: GBA tile download from HuggingFace with disk cache [depends: 21, 23]

### Bottle: GBAHeightEnrichment

- [x] Burst 25: EPSG:4326 to EPSG:3857 coordinate reprojection is accurate [depends: none]
- [x] Burst 26: OSMBuildingReader enriches footprints without OSM height using GBA data [depends: 24, 25]
- [x] Burst 27: Buildings with high GBA variance (>threshold) fall back to type-based defaults [depends: 26]
- [x] Burst 28: heightSource field tracks provenance (osm/gba/default) per building [depends: 26]
