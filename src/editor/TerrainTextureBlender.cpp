/*   Bridge Command 5.0 Ship Simulator
     Copyright (C) 2024 James Packer

     This program is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License version 2 as
     published by the Free Software Foundation

     This program is distributed in the hope that it will be useful,
     but WITHOUT ANY WARRANTY; without even the implied warranty of
     MERCHANTABILITY Or FITNESS For A PARTICULAR PURPOSE.  See the
     GNU General Public License For more details.

     You should have received a copy of the GNU General Public License along
     with this program; if not, write to the Free Software Foundation, Inc.,
     51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA. */

#include "TerrainTextureBlender.hpp"

#include <algorithm>
#include <cmath>
#include <queue>

// Simple hash-based value noise (same approach as building textures)
static uint32_t hashXY(int x, int y) {
    uint32_t h = (uint32_t)(x * 374761393 + y * 668265263);
    h = (h ^ (h >> 13)) * 1274126177;
    return h ^ (h >> 16);
}

static float vnoise(float x, float y, int seed) {
    int ix = (int)std::floor(x), iy = (int)std::floor(y);
    float fx = x - ix, fy = y - iy;
    auto h = [&](int a, int b) -> float {
        return (float)(hashXY(a + seed, b) % 10000) / 10000.0f;
    };
    float v00 = h(ix, iy), v10 = h(ix+1, iy), v01 = h(ix, iy+1), v11 = h(ix+1, iy+1);
    float a = v00 + (v10 - v00) * fx;
    float b = v01 + (v11 - v01) * fx;
    return a + (b - a) * fy;
}

// FBM (fractal Brownian motion) for richer noise
static float fbm(float x, float y, int seed, int octaves = 4) {
    float val = 0.0f, amp = 0.5f, freq = 1.0f;
    for (int i = 0; i < octaves; i++) {
        val += vnoise(x * freq, y * freq, seed + i * 1000) * amp;
        amp *= 0.5f;
        freq *= 2.0f;
    }
    return val;
}

// Terrain type colors (R, G, B)
struct TerrainColor { uint8_t r, g, b; };

// Generate procedural detail color for each terrain type
static TerrainColor grassColor(float nx, float ny, int seed) {
    float n = fbm(nx * 0.8f, ny * 0.8f, seed, 3);
    float n2 = vnoise(nx * 3.0f, ny * 3.0f, seed + 500);
    int r = 65 + (int)(n * 40) + (int)(n2 * 15);
    int g = 100 + (int)(n * 50) + (int)(n2 * 20);
    int b = 40 + (int)(n * 25) + (int)(n2 * 10);
    return { (uint8_t)std::clamp(r, 0, 255),
             (uint8_t)std::clamp(g, 0, 255),
             (uint8_t)std::clamp(b, 0, 255) };
}

static TerrainColor rockColor(float nx, float ny, int seed) {
    float n = fbm(nx * 1.2f, ny * 1.2f, seed + 100, 4);
    float crack = vnoise(nx * 5.0f, ny * 5.0f, seed + 600);
    int base = 120 + (int)(n * 45);
    if (crack < 0.15f) base -= 25; // dark crack lines
    int r = base + 5;
    int g = base;
    int b = base - 5;
    return { (uint8_t)std::clamp(r, 0, 255),
             (uint8_t)std::clamp(g, 0, 255),
             (uint8_t)std::clamp(b, 0, 255) };
}

static TerrainColor sandColor(float nx, float ny, int seed) {
    float n = fbm(nx * 1.5f, ny * 1.5f, seed + 200, 3);
    float grain = vnoise(nx * 8.0f, ny * 8.0f, seed + 700);
    int r = 190 + (int)(n * 30) + (int)(grain * 10);
    int g = 175 + (int)(n * 25) + (int)(grain * 8);
    int b = 140 + (int)(n * 20) + (int)(grain * 6);
    return { (uint8_t)std::clamp(r, 0, 255),
             (uint8_t)std::clamp(g, 0, 255),
             (uint8_t)std::clamp(b, 0, 255) };
}

static TerrainColor dirtColor(float nx, float ny, int seed) {
    float n = fbm(nx * 1.0f, ny * 1.0f, seed + 300, 3);
    float clump = vnoise(nx * 4.0f, ny * 4.0f, seed + 800);
    int r = 110 + (int)(n * 35) + (int)(clump * 15);
    int g = 85 + (int)(n * 30) + (int)(clump * 12);
    int b = 60 + (int)(n * 20) + (int)(clump * 8);
    return { (uint8_t)std::clamp(r, 0, 255),
             (uint8_t)std::clamp(g, 0, 255),
             (uint8_t)std::clamp(b, 0, 255) };
}

void TerrainTextureBlender::blend(uint8_t* textureRGB, int texW, int texH,
                                   const float* heightGrid, int resolution) {
    if (!textureRGB || !heightGrid || texW <= 0 || texH <= 0 || resolution <= 0)
        return;

    int res = resolution;

    // --- Step 1: Compute slope map from heightmap gradient ---
    // slope[r][c] = magnitude of gradient at that pixel (in metres/pixel)
    std::vector<float> slope(res * res, 0.0f);
    for (int r = 1; r < res - 1; r++) {
        for (int c = 1; c < res - 1; c++) {
            float dhdx = (heightGrid[r * res + c + 1] - heightGrid[r * res + c - 1]) * 0.5f;
            float dhdy = (heightGrid[(r + 1) * res + c] - heightGrid[(r - 1) * res + c]) * 0.5f;
            slope[r * res + c] = std::sqrt(dhdx * dhdx + dhdy * dhdy);
        }
    }

    // --- Step 2: Water distance map (BFS from water pixels) ---
    // Distance in pixels from nearest water cell
    std::vector<int> waterDist(res * res, -1);
    std::queue<int> bfsQ;
    for (int i = 0; i < res * res; i++) {
        if (heightGrid[i] <= 0.0f) {
            waterDist[i] = 0;
            bfsQ.push(i);
        }
    }
    const int dx4[] = {-1, 1, 0, 0};
    const int dy4[] = {0, 0, -1, 1};
    while (!bfsQ.empty()) {
        int idx = bfsQ.front(); bfsQ.pop();
        int cr = idx / res, cc = idx % res;
        for (int d = 0; d < 4; d++) {
            int nr = cr + dy4[d], nc = cc + dx4[d];
            if (nr >= 0 && nr < res && nc >= 0 && nc < res) {
                int ni = nr * res + nc;
                if (waterDist[ni] < 0) {
                    waterDist[ni] = waterDist[idx] + 1;
                    bfsQ.push(ni);
                }
            }
        }
    }

    // --- Step 3: For each texture pixel, compute terrain type weights and blend ---
    // Noise seed for breaking up transitions
    const int noiseSeed = 42;

    for (int ty = 0; ty < texH; ty++) {
        for (int tx = 0; tx < texW; tx++) {
            // Map texture pixel to heightmap pixel (bilinear)
            float hmX = (float)tx / texW * (res - 1);
            float hmY = (float)ty / texH * (res - 1);
            int ix = std::min((int)hmX, res - 2);
            int iy = std::min((int)hmY, res - 2);
            float fx = hmX - ix, fy = hmY - iy;

            // Sample height (bilinear)
            float h00 = heightGrid[iy * res + ix];
            float h10 = heightGrid[iy * res + ix + 1];
            float h01 = heightGrid[(iy + 1) * res + ix];
            float h11 = heightGrid[(iy + 1) * res + ix + 1];
            float h = h00 * (1-fx)*(1-fy) + h10 * fx*(1-fy) +
                      h01 * (1-fx)*fy + h11 * fx*fy;

            // Skip water pixels
            if (h <= 0.0f) continue;

            // Sample slope (bilinear)
            float s00 = slope[iy * res + ix];
            float s10 = slope[iy * res + ix + 1];
            float s01 = slope[(iy + 1) * res + ix];
            float s11 = slope[(iy + 1) * res + ix + 1];
            float s = s00 * (1-fx)*(1-fy) + s10 * fx*(1-fy) +
                      s01 * (1-fx)*fy + s11 * fx*fy;

            // Sample water distance (nearest neighbor, convert to approx metres)
            int wdIdx = std::min(iy, res - 1) * res + std::min(ix, res - 1);
            float wDistPx = (waterDist[wdIdx] >= 0) ? (float)waterDist[wdIdx] : 999.0f;
            // Approximate metres: assume heightmap spans ~10km for 1025 pixels ~ 10m/pixel
            float mPerPixel = 10.0f; // rough estimate
            float wDistM = wDistPx * mPerPixel;

            // --- Compute terrain type weights ---
            float wGrass = 0.0f, wRock = 0.0f, wSand = 0.0f, wDirt = 0.0f;

            // Slope: steep (>3 m/px) -> rock, moderate -> dirt, flat -> grass
            float slopeNorm = std::min(s / 5.0f, 1.0f); // 0=flat, 1=very steep
            wRock = slopeNorm;

            // Water proximity: close to water -> sand/beach
            if (wDistM < 30.0f) {
                float sandW = 1.0f - wDistM / 30.0f;
                wSand = sandW * (1.0f - slopeNorm); // sand only on flat areas
            } else if (wDistM < 100.0f) {
                float t = (wDistM - 30.0f) / 70.0f;
                wDirt = (1.0f - t) * 0.5f * (1.0f - slopeNorm);
            }

            // Remaining weight goes to grass
            float used = wRock + wSand + wDirt;
            wGrass = std::max(0.0f, 1.0f - used);

            // Elevation modulation: high elevation shifts grass->rock
            if (h > 50.0f) {
                float highFactor = std::min((h - 50.0f) / 100.0f, 0.5f);
                float grassShift = wGrass * highFactor;
                wGrass -= grassShift;
                wRock += grassShift;
            }

            // Noise modulation to break up transitions
            float noiseX = (float)tx * 0.15f;
            float noiseY = (float)ty * 0.15f;
            float nMod = fbm(noiseX, noiseY, noiseSeed, 3);
            // Perturb weights slightly with noise
            wGrass += (nMod - 0.5f) * 0.15f;
            wRock += (vnoise(noiseX * 0.7f, noiseY * 0.7f, noiseSeed + 50) - 0.5f) * 0.1f;
            wSand += (vnoise(noiseX * 0.9f, noiseY * 0.9f, noiseSeed + 80) - 0.5f) * 0.08f;

            // Clamp and renormalize
            wGrass = std::max(0.0f, wGrass);
            wRock = std::max(0.0f, wRock);
            wSand = std::max(0.0f, wSand);
            wDirt = std::max(0.0f, wDirt);
            float wTotal = wGrass + wRock + wSand + wDirt;
            if (wTotal > 0.001f) {
                wGrass /= wTotal; wRock /= wTotal;
                wSand /= wTotal; wDirt /= wTotal;
            }

            // Generate detail colors (tiling coordinates based on texture position)
            // ~10m repeat: tex pixel maps to world, use fractional tiling coords
            float tileX = (float)tx * 0.3f;
            float tileY = (float)ty * 0.3f;

            TerrainColor cGrass = grassColor(tileX, tileY, 1000);
            TerrainColor cRock = rockColor(tileX, tileY, 2000);
            TerrainColor cSand = sandColor(tileX, tileY, 3000);
            TerrainColor cDirt = dirtColor(tileX, tileY, 4000);

            // Weighted detail color
            float dr = cGrass.r * wGrass + cRock.r * wRock + cSand.r * wSand + cDirt.r * wDirt;
            float dg = cGrass.g * wGrass + cRock.g * wRock + cSand.g * wSand + cDirt.g * wDirt;
            float db = cGrass.b * wGrass + cRock.b * wRock + cSand.b * wSand + cDirt.b * wDirt;

            // Blend with satellite: lerp(satellite, detail, alpha)
            // Alpha: moderate blend to preserve satellite features
            float alpha = 0.35f;
            // Add noise modulation to alpha
            alpha *= (0.7f + fbm(noiseX * 0.5f, noiseY * 0.5f, noiseSeed + 999, 2) * 0.6f);
            alpha = std::clamp(alpha, 0.1f, 0.55f);

            int pixIdx = (ty * texW + tx) * 3;
            float sr = textureRGB[pixIdx], sg = textureRGB[pixIdx + 1], sb = textureRGB[pixIdx + 2];

            textureRGB[pixIdx]     = (uint8_t)std::clamp(sr * (1.0f - alpha) + dr * alpha, 0.0f, 255.0f);
            textureRGB[pixIdx + 1] = (uint8_t)std::clamp(sg * (1.0f - alpha) + dg * alpha, 0.0f, 255.0f);
            textureRGB[pixIdx + 2] = (uint8_t)std::clamp(sb * (1.0f - alpha) + db * alpha, 0.0f, 255.0f);
        }
    }
}
