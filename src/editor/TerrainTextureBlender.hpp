#pragma once

#include <cstdint>
#include <vector>

// CPU-side terrain detail texturing (Phase A).
// Enhances satellite texture.png by blending procedural detail textures
// based on slope, elevation, water proximity, and noise.
class TerrainTextureBlender {
public:
    // Modify satellite texture in-place by blending procedural detail textures
    // based on slope, elevation, water proximity, noise, and optional land use.
    // textureRGB: RGB interleaved satellite image (modified in-place)
    // texW, texH: texture dimensions
    // heightGrid: row-major float elevation (row 0 = north, matching texture layout)
    // resolution: heightGrid width/height
    // landUseGrid: optional land use type per heightmap pixel (nullable).
    //   Values are LandUseType enum cast to uint8_t. If null, uses topographic-only.
    static void blend(uint8_t* textureRGB, int texW, int texH,
                      const float* heightGrid, int resolution,
                      const uint8_t* landUseGrid = nullptr);

    // Generate terrain normal map (RGB, tangent-space) from heightmap via Sobel filter.
    // Output is allocated by caller: outRGB must be resolution*resolution*3 bytes.
    // worldWidth/worldDepth in metres for correct scale conversion.
    static void generateNormalMap(uint8_t* outRGB, const float* heightGrid,
                                  int resolution, float worldWidth, float worldDepth);

    // Generate terrain roughness map from land use classification.
    // Output is allocated by caller: outRGB must be resolution*resolution*3 bytes.
    static void generateRoughnessMap(uint8_t* outRGB, const float* heightGrid,
                                      const uint8_t* landUseGrid, int resolution);
};
