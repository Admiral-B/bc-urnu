# Coastal Terrain Realism -- Research & Implementation Guide

## Current Pipeline

Heightmap generated from S-57 chart soundings (IDW interpolation), Copernicus DEM, BlueTopo bathymetry, and OSM coastline polygons. Resolution: 1025-2049 pixels for ~10km areas = **5-10m/pixel**. Satellite texture from ESRI World Imagery tiles blended with procedural terrain detail (FBM noise, slope-based splatting, height-priority blending). Beach ramp via chamfer distance transform (variable 4-10 pixel width). Dock/quay edges get sharp concrete transition. Single mesh up to 1024x1024 vertices, one material, one texture.

**Key files:**
- `src/HeightmapGenerator.cpp` -- Heightmap from chart/DEM data
- `src/editor/TerrainTextureBlender.cpp` -- Texture blending (FBM noise, slope/height splatting)
- `src/graphics/wicked/WickedTerrainNode.cpp` -- Mesh generation for WE
- `src/editor/EditorApp.cpp` -- Beach ramp, coastal smoothing (~line 2300)
- `src/BarrierFloodFill.cpp` -- Barrier flood-fill for dams/barrages

## What's Implemented

- **Variable beach ramp**: 4-10 pixel width via hash noise (no longer uniform)
- **Dock waterfront mask**: OSM dock/marina water dilated 3px -> quay wall pixels get sharp transition + concrete texture
- **Wet sand zone**: Albedo darkened 30% within 15m of waterline
- **Macro variation**: 3-octave FBM colour modulation breaks tiling at bridge height
- **Height-priority blending**: Sand fills cracks, grass on bumps (depth-based not linear)
- **Muted procedural colours**: Blend alpha reduced (0.30 unclassified, 0.45 land-use) so satellite imagery dominates
- **Wider sand transition**: 40-80m noise-modulated (was fixed 30m) with dirt transition band
- **Terrain undulation** (chart path only): FBM rolling hills, skips land <5m, quadratic amplitude
- **Coastal fractal noise** (chart path only): +/-1.5m FBM within 250m of coast
- **Thermal erosion** (chart path only): 80 iterations, creates gullies/sediment fans
- **19 land-use types**: Including Waterfront (concrete), Wetland, Mud, Shingle, TidalFlat

## Remaining Limitations

- **10m/pixel**: Coastal features smaller than 10m (coves, rock outcrops, harbour steps) are lost
- **Smooth coastline**: Rasterized at heightmap resolution, no fractal detail
- **No tri-planar**: Stretched textures on steep coastal cliffs
- **No erosion in EditorApp path**: Undulation, coastal noise, thermal erosion only in HeightmapGenerator (chart converter)
- **Single mesh**: Uniform vertex density everywhere, no adaptive LOD near shore

## Techniques Ranked by Impact/Effort

### Tier 1: Quick Wins (CPU-side, existing files)

#### 1. Fractal Noise on Coastline Heights

Add FBM perturbation to heightmap within ~200m of coastline. Creates irregular coves, rocky points, natural beach edges.

In `HeightmapGenerator::generate()`, after initial height computation:
```cpp
float coastDist = distanceToCoastline(x, y);
if (coastDist < 200.0f) {
    float noise = fbm(lon * 500, lat * 500, seed, 6); // [-0.5, 0.5]
    float influence = 1.0f - (coastDist / 200.0f);
    influence *= influence; // quadratic falloff
    height += (noise - 0.5f) * influence * 3.0f; // +/- 3m max
}
```

Preserves chart accuracy for navigation but adds visual complexity. Real coastlines are fractal (Richardson, 1961). ~50 lines.

#### 2. Wet Sand Zone

Darken albedo and reduce roughness near waterline. Physical basis: wet surfaces reduce albedo ~30%, roughness ~50% (Lagarde 2013).

In `TerrainTextureBlender::blend()`:
```cpp
if (wDistM < 15.0f) {
    float wetness = 1.0f - wDistM / 15.0f;
    wetness *= wetness;
    dr *= (1.0f - wetness * 0.3f);
    dg *= (1.0f - wetness * 0.3f);
    db *= (1.0f - wetness * 0.3f);
}
```

Also reduce roughness in `generateRoughnessMap()` for wet-sand reflectivity. ~20 lines total.

#### 3. Macro Variation (Anti-Tiling)

Large-scale color modulation breaks up visible noise repetition when viewed from bridge height.

```cpp
float macroNoise = fbm(tx * 0.01f, ty * 0.01f, 99999, 3);
float macroShift = (macroNoise - 0.5f) * 30.0f; // +/- 15 RGB
dr += macroShift; dg += macroShift * 0.8f; db += macroShift * 0.6f;
```

~10 lines in `TerrainTextureBlender::blend()`.

#### 4. Domain Warping

Warp heightmap sampling coordinates with FBM for organic terrain distortion:
```cpp
float warpX = fbm(lon * 500, lat * 500, seed1, 4) * 0.0002;
float warpY = fbm(lon * 500, lat * 500, seed2, 4) * 0.0002;
float height = sampleHeightmap(lon + warpX, lat + warpY);
```

Creates natural-looking tectonic deformation. ~10 lines.

### Tier 2: Medium Effort (Pipeline changes)

#### 5. Fractal Subdivision of Coastline Polygons

Before rasterizing coastlines, apply recursive midpoint displacement to polylines:
1. For each segment, insert midpoint
2. Displace perpendicular by random amount * segment_length * roughness
3. Repeat 3-4 iterations (roughness ~0.5 for natural coastlines)

Different from #1 (which perturbs heights). This changes the actual coastline shape at sub-chart resolution. ~80 lines in `HeightmapGenerator`.

#### 6. Higher Resolution Coastal Strip

Generate a second 2049x2049 heightmap covering just the coastal strip (~1-2km inland). Load as a separate `WickedTerrainNode` overlaid on the main terrain. Microsoft Flight Simulator uses this approach for handcrafted airport areas.

Reuses existing pipeline. Main work: coordinate alignment and overlap blending.

#### 7. Thermal Erosion Pass

Post-process heightmap: material slides from steep slopes to adjacent lower cells. 50-100 iterations creates realistic gullies, sediment fans, smoothed beach profiles.

```cpp
for each iteration:
    for each cell:
        maxDiff = max height difference to neighbors
        if (maxDiff > talus_angle * cell_size):
            transfer = (maxDiff - talus_angle * cell_size) * 0.5
            lower self, raise lowest neighbor
```

~100 lines, runs once during world generation. Reference: github.com/dandrino/terrain-erosion-3-ways.

#### 8. Height-Based Texture Priority Blending

Replace linear weight interpolation with height-priority blending. Sand fills cracks between rocks, grass grows on bumps.

```cpp
float depthA = heightA + weightA;
float depthB = heightB + weightB;
float ma = max(depthA, depthB) - blendDepth;
float bA = max(depthA - ma, 0); float bB = max(depthB - ma, 0);
finalColor = (colorA * bA + colorB * bB) / (bA + bB);
```

Requires per-material height values (can be derived from noise). ~30 lines change in blend().

### Tier 3: Significant Effort

#### 9. Adaptive Mesh Density Near Coastline

Non-uniform vertex distribution: full resolution within N pixels of coastline, step=2 or step=4 further away. Doubles effective shore resolution without increasing total vertex count.

Significant refactor of `WickedTerrainNode::createTerrainMesh()`. Requires T-junction stitching between resolution zones.

#### 10. GPU Detail Texture Overlay

Blend tiled PBR detail textures (sand grains, rock, grass) with satellite base at close range. Requires custom HLSL material shader or migration to WE's virtual texture pipeline.

#### 11. Tri-Planar Mapping for Cliffs

Project textures from X/Y/Z axes simultaneously, blend by surface normal. Eliminates stretched textures on steep coastal cliffs. Requires custom shader integration with WE's PBR pipeline.

### Tier 4: Major Architecture Change

#### 12. Migrate to WE's Built-In Terrain System

WE has a full terrain system (`wi::terrain::Terrain`) with:
- 67x67 vertex chunks with automatic LOD (up to 16x reduction)
- Virtual texturing: GPU-generated texture atlas (BC1/BC3/BC5 compressed)
- 4 material regions: base, slope, low-altitude, high-altitude (blended by weight)
- Procedural modifiers: Perlin noise, Voronoi, heightmap sampler
- Props: Random object placement per chunk
- Grass: HairParticleSystem integration
- Physics: Heightfield collision per chunk
- Spline modifiers: Roads/rivers that flatten terrain

**Trade-off**: Full LOD + virtual texturing + grass + props for free, but requires converting heightmap data to WE's `HeightmapModifier` format, mapping terrain types to 4-region material system, and accepting WE's chunked architecture. Weeks of work.

## WE Terrain System Details

Not currently used. BC builds a custom `MeshComponent` in `WickedTerrainNode`. Key WE terrain types:

| Feature | WE Terrain | Current BC |
|---|---|---|
| LOD | Automatic, 6+ levels | None |
| Chunks | 67x67 vertices | Single 1024x1024 mesh |
| Texturing | Virtual texture atlas | Single satellite PNG |
| Materials | 4 region layers | 1 material |
| Grass | Built-in particle | None |
| Physics | Per-chunk collision | Single body |

Files: `WickedEngine/wiTerrain.cpp` (2600 lines), `WickedEngine/shaders/terrainVirtualTextureUpdateCS.hlsl`.

## Beach Ramp System (Current)

Two-pass chamfer distance transform from coastline boundary pixels:
- Forward pass (top-left to bottom-right): propagates distance from coast
- Backward pass (bottom-right to top-left): refines to Euclidean distance
- Land side: smoothstep from 0.3m to DEM height over 4-10 pixels (hash noise per-pixel)
- Water side: smoothstep from -0.2m to charted depth over same variable width
- Island mask pixels included (islands get natural beach transitions)
- **Dock edge pixels skipped**: quay walls get sharp 1px transition, land raised to 2m minimum

## Dock Waterfront Mask (Current)

Man-made waterfront detection from OSM dock/marina water polygons (`OSMWaterReader` type="dock"/"marina"):

1. During water polygon subtraction, dock/marina water pixels flagged in `dockWaterMask`
2. `dockEdgeMask` = dockWaterMask dilated 3px, intersected with landMask
3. Dock edge pixels: skip beach ramp (sharp transition), land clamped to 2m
4. `landUseGrid` overridden to `Waterfront` (type 19) for concrete texture + roughness 0.50

Files: `EditorApp.cpp` (mask creation + beach ramp skip), `TerrainTextureBlender.cpp` (concrete colour), `OSMLandUseReader.hpp` (enum).

## Barrier System (Current)

`BarrierFloodFill.cpp` handles dams, barrages, breakwaters:
- Rasterizes barrier polylines via Bresenham
- Snaps endpoints to nearby land (~390m search radius)
- 3-pass dilation (barrier becomes ~7 pixels wide)
- Raises barrier pixels to 2.0m
- Does NOT convert enclosed water to land (barrages protect water)

## References

- Fractal coastlines: Richardson (1961), Red Blob Games terrain-from-noise
- Wet surfaces: Lagarde (2013) physically-based wet rendering
- Height-based splatting: gamedeveloper.com/advanced-terrain-texture-splatting
- Erosion: github.com/dandrino/terrain-erosion-3-ways
- Tri-planar: Golus (2017) normal-mapping-for-a-triplanar-shader
- WE terrain: github.com/turanszkij/WickedEngine wiTerrain.cpp
- MSFS terrain: GDC 2021 "Designing the Terrain System of Flight Simulator"
- Crest shorelines: crest.readthedocs.io shallows-and-shorelines
