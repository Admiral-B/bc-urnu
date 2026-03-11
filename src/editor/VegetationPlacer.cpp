#include "VegetationPlacer.hpp"
#include "../libs/stb/stb_image_write.h"

#include <cmath>
#include <algorithm>
#include <sstream>
#include <iomanip>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Knuth multiplicative hash for deterministic pseudo-random
static uint32_t hashU32(uint32_t x) {
    x = ((x >> 16) ^ x) * 0x45d9f3b;
    x = ((x >> 16) ^ x) * 0x45d9f3b;
    x = (x >> 16) ^ x;
    return x;
}

// Density: trees per hectare (10000 m^2) for each land use type
float VegetationPlacer::densityForLandUse(LandUseType type) {
    switch (type) {
    case LandUseType::Forest:       return 80.0f;  // Dense woodland
    case LandUseType::Heath:        return 15.0f;  // Scattered scrub
    case LandUseType::Grass:        return 5.0f;   // Parks -- occasional trees
    case LandUseType::Farmland:     return 2.0f;   // Hedgerow trees only
    case LandUseType::Allotments:   return 3.0f;   // Fruit trees
    case LandUseType::Cemetery:     return 12.0f;  // Mature trees common
    case LandUseType::Residential:  return 8.0f;   // Garden/street trees
    default:                        return 0.0f;    // No trees
    }
}

TreeSpecies VegetationPlacer::selectSpecies(LandUseType type, double latitude, uint32_t hash) {
    double absLat = std::abs(latitude);

    // Tropical palms below ~35 degrees
    if (absLat < 35.0 && (hash % 100) < 30) {
        return TreeSpecies::Palm;
    }

    // Heath/scrub -> mostly shrubs
    if (type == LandUseType::Heath) {
        return (hash % 100 < 70) ? TreeSpecies::Shrub : TreeSpecies::Conifer;
    }

    // Forest: mix of deciduous and conifer, more conifer at high latitudes
    if (type == LandUseType::Forest) {
        int coniferChance = (absLat > 55.0) ? 60 : (absLat > 45.0) ? 40 : 20;
        return ((int)(hash % 100) < coniferChance) ? TreeSpecies::Conifer : TreeSpecies::Deciduous;
    }

    // Residential/parks: mostly deciduous
    if (type == LandUseType::Residential || type == LandUseType::Grass ||
        type == LandUseType::Cemetery) {
        return (hash % 100 < 85) ? TreeSpecies::Deciduous : TreeSpecies::Conifer;
    }

    // Farmland: hedgerow trees, deciduous
    return TreeSpecies::Deciduous;
}

void VegetationPlacer::generate(const uint8_t* landUseGrid,
                                 const float* heightGrid,
                                 int resolution,
                                 double minLat, double maxLat,
                                 double minLon, double maxLon,
                                 int maxTrees,
                                 ProgressCallback progress) {
    trees.clear();
    if (!landUseGrid || !heightGrid || resolution < 10) return;

    double latRange = maxLat - minLat;
    double lonRange = maxLon - minLon;
    double midLat = (minLat + maxLat) * 0.5;
    double cosLat = std::cos(midLat * M_PI / 180.0);

    // Cell size in metres (approximate)
    double cellLatM = latRange * 110540.0 / resolution;
    double cellLonM = lonRange * 111320.0 * cosLat / resolution;
    double cellAreaHa = (cellLatM * cellLonM) / 10000.0; // hectares per cell

    if (progress) progress("Placing vegetation...");

    // First pass: count eligible cells per density to estimate total,
    // then adjust stride to hit maxTrees target.
    // Use a grid-based approach with jittered positions (fast Poisson-disk approximation).

    // Minimum spacing in grid cells between trees (prevents clustering)
    // We'll use sub-cell jitter within a coarser grid.

    // Collect candidate cells with their density
    struct CandidateCell {
        int px, py;
        float density; // trees per hectare
        LandUseType type;
    };
    std::vector<CandidateCell> candidates;
    candidates.reserve(resolution * resolution / 4);

    for (int py = 0; py < resolution; py++) {
        for (int px = 0; px < resolution; px++) {
            int idx = py * resolution + px;
            float h = heightGrid[idx];
            if (h < 0.5f) continue; // Water or near-shoreline

            auto luType = static_cast<LandUseType>(landUseGrid[idx]);
            float density = densityForLandUse(luType);
            if (density <= 0.0f) continue;

            // Skip steep slopes (approximate from neighbors)
            if (py > 0 && py < resolution - 1 && px > 0 && px < resolution - 1) {
                float hL = heightGrid[idx - 1];
                float hR = heightGrid[idx + 1];
                float hU = heightGrid[idx - resolution];
                float hD = heightGrid[idx + resolution];
                float dhdx = std::abs(hR - hL) / (2.0f * (float)cellLonM);
                float dhdy = std::abs(hD - hU) / (2.0f * (float)cellLatM);
                float slope = std::atan(std::sqrt(dhdx * dhdx + dhdy * dhdy)) * 180.0f / (float)M_PI;
                if (slope > 35.0f) continue; // Too steep for trees
            }

            candidates.push_back({px, py, density, luType});
        }
    }

    if (candidates.empty()) {
        if (progress) progress("No suitable terrain for vegetation");
        return;
    }

    // Expected total trees if we place at full density
    double totalExpected = 0;
    for (const auto& c : candidates) {
        totalExpected += c.density * cellAreaHa;
    }

    // Scale factor to hit maxTrees target
    float scaleFactor = (totalExpected > maxTrees) ? (float)(maxTrees / totalExpected) : 1.0f;

    if (progress) progress("Scattering " + std::to_string((int)(totalExpected * scaleFactor)) + " trees...");

    // Place trees: for each candidate cell, probabilistically place a tree
    trees.reserve(std::min((int)(totalExpected * scaleFactor) + 1000, maxTrees + 1000));

    for (const auto& c : candidates) {
        // Expected trees in this cell
        float expected = c.density * (float)cellAreaHa * scaleFactor;

        // Deterministic hash from cell position
        uint32_t cellHash = hashU32((uint32_t)(c.py * 65537 + c.px));

        // Integer part: guaranteed trees. Fractional part: probabilistic.
        int nTrees = (int)expected;
        float frac = expected - (float)nTrees;
        if ((cellHash % 1000) < (uint32_t)(frac * 1000.0f)) nTrees++;

        for (int t = 0; t < nTrees && (int)trees.size() < maxTrees; t++) {
            uint32_t tHash = hashU32(cellHash + (uint32_t)t * 7919);

            // Jitter within cell (0.1 to 0.9 of cell size to avoid edges)
            float jx = 0.1f + 0.8f * ((tHash >> 0) & 0xFFF) / 4096.0f;
            float jy = 0.1f + 0.8f * ((tHash >> 12) & 0xFFF) / 4096.0f;

            double lat = maxLat - latRange * (c.py + jy) / resolution;
            double lon = minLon + lonRange * (c.px + jx) / resolution;

            // Bilinear height interpolation
            float fx = c.px + jx;
            float fy = c.py + jy;
            int ix = std::min((int)fx, resolution - 2);
            int iy = std::min((int)fy, resolution - 2);
            float tx = fx - ix;
            float ty = fy - iy;
            float h00 = heightGrid[iy * resolution + ix];
            float h10 = heightGrid[iy * resolution + ix + 1];
            float h01 = heightGrid[(iy + 1) * resolution + ix];
            float h11 = heightGrid[(iy + 1) * resolution + ix + 1];
            float h = h00 * (1 - tx) * (1 - ty) + h10 * tx * (1 - ty) +
                      h01 * (1 - tx) * ty + h11 * tx * ty;

            if (h < 0.5f) continue; // Safety check after interpolation

            TreePlacement tp;
            tp.longitude = lon;
            tp.latitude = lat;
            tp.height = h;
            tp.scale = 0.6f + 1.0f * ((tHash >> 24) & 0xFF) / 255.0f; // 0.6 to 1.6
            tp.rotation = 360.0f * ((tHash >> 8) & 0xFFF) / 4096.0f;
            tp.species = selectSpecies(c.type, lat, tHash);

            // Shrubs are shorter
            if (tp.species == TreeSpecies::Shrub) {
                tp.scale *= 0.5f;
            }

            trees.push_back(tp);
        }
    }

    if (progress) progress("Placed " + std::to_string(trees.size()) + " trees");
}

std::string VegetationPlacer::generateTreesIni(const std::string& textureFile) const {
    std::ostringstream oss;
    oss << std::fixed;

    oss << "Number=" << trees.size() << "\n";
    oss << "TextureFile=" << textureFile << "\n";
    oss << "\n";

    oss.precision(7);
    for (size_t i = 0; i < trees.size(); i++) {
        const auto& t = trees[i];
        size_t n = i + 1;
        oss << "Long(" << n << ")=" << t.longitude << "\n";
        oss << "Lat(" << n << ")=" << t.latitude << "\n";
        oss.precision(1);
        oss << "Height(" << n << ")=" << t.height << "\n";
        oss.precision(2);
        oss << "Scale(" << n << ")=" << t.scale << "\n";
        oss.precision(1);
        oss << "Rotation(" << n << ")=" << t.rotation << "\n";
        oss << "Species(" << n << ")=" << (int)t.species << "\n";
        oss.precision(7);
    }

    return oss.str();
}

// ---- Procedural tree atlas texture generation ----

// Simple 2D distance from center for circular shapes
static float circDist(float cx, float cy, float x, float y) {
    float dx = x - cx, dy = y - cy;
    return std::sqrt(dx * dx + dy * dy);
}

std::vector<uint8_t> VegetationPlacer::generateAtlasTexture(int cellSize) {
    int atlasSize = cellSize * 2;
    // RGBA output
    std::vector<uint8_t> pixels(atlasSize * atlasSize * 4, 0);

    auto setPixel = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        if (x < 0 || x >= atlasSize || y < 0 || y >= atlasSize) return;
        int idx = (y * atlasSize + x) * 4;
        pixels[idx] = r; pixels[idx+1] = g; pixels[idx+2] = b; pixels[idx+3] = a;
    };

    // Deterministic noise
    auto pHash = [](int x, int y, int seed) -> uint32_t {
        uint32_t h = (uint32_t)(x * 374761393 + y * 668265263 + seed * 1274126177);
        h = (h ^ (h >> 13)) * 1274126177;
        return (h ^ (h >> 16));
    };

    // ---- Species 0: Deciduous (top-left quadrant) ----
    // Round canopy on a short trunk
    {
        int ox = 0, oy = 0;
        float cx = cellSize * 0.5f, cy = cellSize * 0.35f; // canopy center
        float canopyR = cellSize * 0.38f;
        float trunkW = cellSize * 0.06f;
        float trunkTop = cellSize * 0.55f;

        for (int ly = 0; ly < cellSize; ly++) {
            for (int lx = 0; lx < cellSize; lx++) {
                float fx = (float)lx, fy = (float)ly;
                float d = circDist(cx, cy, fx, fy);
                int noise = (int)(pHash(lx, ly, 100) % 20) - 10;

                // Trunk
                if (fy > trunkTop && std::abs(fx - cx) < trunkW) {
                    int br = 80 + noise/2;
                    setPixel(ox + lx, oy + ly, (uint8_t)(br+15), (uint8_t)(br-5), (uint8_t)(br-20), 255);
                    continue;
                }

                // Canopy (with irregular edge)
                float edgeNoise = (float)(pHash(lx/3, ly/3, 200) % 100) / 100.0f * canopyR * 0.15f;
                if (d < canopyR - edgeNoise) {
                    // Depth-based shading: darker at edges
                    float t = d / canopyR;
                    int g = (int)(100 + 80 * (1.0f - t * t)) + noise;
                    int r = (int)(40 + 30 * (1.0f - t)) + noise/2;
                    int b = (int)(20 + 20 * (1.0f - t)) + noise/3;
                    g = std::max(0, std::min(255, g));
                    r = std::max(0, std::min(255, r));
                    b = std::max(0, std::min(255, b));
                    setPixel(ox + lx, oy + ly, (uint8_t)r, (uint8_t)g, (uint8_t)b, 255);
                }
            }
        }
    }

    // ---- Species 1: Conifer (top-right quadrant) ----
    // Triangular/conical shape, dark green
    {
        int ox = cellSize, oy = 0;
        float cx = cellSize * 0.5f;
        float trunkW = cellSize * 0.04f;
        float treeTop = cellSize * 0.05f;
        float treeBot = cellSize * 0.75f;
        float maxHalfW = cellSize * 0.30f;

        for (int ly = 0; ly < cellSize; ly++) {
            for (int lx = 0; lx < cellSize; lx++) {
                float fx = (float)lx, fy = (float)ly;
                int noise = (int)(pHash(lx, ly, 300) % 16) - 8;

                // Trunk (below tree)
                if (fy > treeBot && std::abs(fx - cx) < trunkW) {
                    int br = 70 + noise/2;
                    setPixel(ox + lx, oy + ly, (uint8_t)(br+10), (uint8_t)(br-8), (uint8_t)(br-18), 255);
                    continue;
                }

                // Conical canopy
                if (fy >= treeTop && fy <= treeBot) {
                    float t = (fy - treeTop) / (treeBot - treeTop); // 0 at top, 1 at bottom
                    float halfW = maxHalfW * t;
                    float edgeNoise = (float)(pHash(lx/2, ly/2, 400) % 100) / 100.0f * halfW * 0.2f;
                    if (std::abs(fx - cx) < halfW + edgeNoise) {
                        // Layered shading (sawtooth for branch layers)
                        float layer = std::fmod(fy * 0.08f, 1.0f);
                        int g = (int)(50 + 60 * (0.5f + 0.5f * layer)) + noise;
                        int r = (int)(15 + 20 * layer) + noise/3;
                        int b = (int)(10 + 15 * layer) + noise/4;
                        g = std::max(0, std::min(255, g));
                        r = std::max(0, std::min(255, r));
                        b = std::max(0, std::min(255, b));
                        setPixel(ox + lx, oy + ly, (uint8_t)r, (uint8_t)g, (uint8_t)b, 255);
                    }
                }
            }
        }
    }

    // ---- Species 2: Shrub (bottom-left quadrant) ----
    // Wide, low, irregular bush shape
    {
        int ox = 0, oy = cellSize;
        float cx = cellSize * 0.5f, cy = cellSize * 0.45f;
        float rX = cellSize * 0.42f; // wider than tall
        float rY = cellSize * 0.32f;

        for (int ly = 0; ly < cellSize; ly++) {
            for (int lx = 0; lx < cellSize; lx++) {
                float fx = (float)lx, fy = (float)ly;
                float dx = (fx - cx) / rX, dy = (fy - cy) / rY;
                float d = std::sqrt(dx * dx + dy * dy);
                int noise = (int)(pHash(lx, ly, 500) % 24) - 12;

                float edgeNoise = (float)(pHash(lx/2, ly/2, 600) % 100) / 100.0f * 0.2f;
                if (d < 1.0f - edgeNoise && fy > cellSize * 0.15f) {
                    float t = d;
                    int g = (int)(70 + 80 * (1.0f - t * t)) + noise;
                    int r = (int)(50 + 40 * (1.0f - t)) + noise/2;
                    int b = (int)(15 + 15 * (1.0f - t)) + noise/3;
                    g = std::max(0, std::min(255, g));
                    r = std::max(0, std::min(255, r));
                    b = std::max(0, std::min(255, b));
                    setPixel(ox + lx, oy + ly, (uint8_t)r, (uint8_t)g, (uint8_t)b, 255);
                }
            }
        }
    }

    // ---- Species 3: Palm (bottom-right quadrant) ----
    // Tall curved trunk with frond cluster at top
    {
        int ox = cellSize, oy = cellSize;
        float cx = cellSize * 0.5f;
        float trunkBot = cellSize * 0.9f;
        float trunkTop = cellSize * 0.25f;
        float trunkW = cellSize * 0.035f;
        float frondCenter = trunkTop;

        for (int ly = 0; ly < cellSize; ly++) {
            for (int lx = 0; lx < cellSize; lx++) {
                float fx = (float)lx, fy = (float)ly;
                int noise = (int)(pHash(lx, ly, 700) % 14) - 7;

                // Trunk (slight curve)
                float trunkT = (fy - trunkTop) / (trunkBot - trunkTop);
                if (trunkT >= 0.0f && trunkT <= 1.0f) {
                    float curve = std::sin(trunkT * 3.14159f * 0.3f) * cellSize * 0.04f;
                    if (std::abs(fx - cx - curve) < trunkW * (0.7f + 0.3f * trunkT)) {
                        int br = 140 + noise;
                        setPixel(ox + lx, oy + ly, (uint8_t)(br+5), (uint8_t)(br-10), (uint8_t)(br-30), 255);
                        continue;
                    }
                }

                // Fronds (radiating lines from top of trunk)
                float fdx = fx - cx, fdy = fy - frondCenter;
                float fdist = std::sqrt(fdx * fdx + fdy * fdy);
                float frondR = cellSize * 0.35f;
                if (fdist > 0 && fdist < frondR) {
                    float angle = std::atan2(fdy, fdx);
                    // 6-8 fronds radiating outward
                    float frondAngle = std::fmod(angle + 3.14159f, 3.14159f * 2.0f / 7.0f);
                    float frondWidth = cellSize * 0.04f * (1.0f - fdist / frondR);
                    float frondCenter2 = 3.14159f * 2.0f / 14.0f; // center of each frond segment
                    float angDist = std::abs(frondAngle - frondCenter2);
                    if (angDist * fdist < frondWidth) {
                        float t = fdist / frondR;
                        int g = (int)(80 + 100 * (1.0f - t)) + noise;
                        int r = (int)(30 + 40 * (1.0f - t)) + noise/2;
                        int b = (int)(10 + 15 * (1.0f - t)) + noise/3;
                        g = std::max(0, std::min(255, g));
                        r = std::max(0, std::min(255, r));
                        b = std::max(0, std::min(255, b));
                        setPixel(ox + lx, oy + ly, (uint8_t)r, (uint8_t)g, (uint8_t)b, 255);
                    }
                }
            }
        }
    }

    return pixels;
}

bool VegetationPlacer::writeAtlasPNG(const std::string& path, const std::vector<uint8_t>& rgbaData,
                                      int width, int height) {
    if (rgbaData.size() != static_cast<size_t>(width * height * 4)) return false;
    return stbi_write_png(path.c_str(), width, height, 4, rgbaData.data(), width * 4) != 0;
}
