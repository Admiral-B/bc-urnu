#pragma once

#include <cstdint>
#include <vector>

// CPU-side terrain detail texturing (Phase A).
// Enhances satellite texture.png by blending procedural detail textures
// based on slope, elevation, water proximity, and noise.
class TerrainTextureBlender {
public:
    // Modify satellite texture in-place by blending terrain-type detail.
    // textureRGB: RGB interleaved satellite image (modified in-place)
    // texW, texH: texture dimensions
    // heightGrid: row-major float elevation (row 0 = north, matching texture layout)
    // resolution: heightGrid width/height
    static void blend(uint8_t* textureRGB, int texW, int texH,
                      const float* heightGrid, int resolution);
};
