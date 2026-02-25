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

#ifdef WITH_GDAL

#include "HeightmapGenerator.hpp"
#include "BarrierFloodFill.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "libs/stb/stb_image_write.h"

#include "gdal_priv.h"

#include <cmath>
#include <algorithm>
#include <limits>
#include <iostream>
#include <queue>

// ── Noise functions for terrain perturbation ──────────────────────────────

static uint32_t hmHash(int x, int y) {
    uint32_t h = (uint32_t)(x * 374761393 + y * 668265263);
    h = (h ^ (h >> 13)) * 1274126177;
    return h ^ (h >> 16);
}

static float hmVnoise(float x, float y, int seed) {
    int ix = (int)std::floor(x), iy = (int)std::floor(y);
    float fx = x - ix, fy = y - iy;
    fx = fx * fx * (3.0f - 2.0f * fx); // smoothstep
    fy = fy * fy * (3.0f - 2.0f * fy);
    auto h = [&](int a, int b) -> float {
        return (float)(hmHash(a + seed, b) % 10000) / 10000.0f;
    };
    float v00 = h(ix, iy), v10 = h(ix+1, iy), v01 = h(ix, iy+1), v11 = h(ix+1, iy+1);
    float a = v00 + (v10 - v00) * fx;
    float b = v01 + (v11 - v01) * fx;
    return a + (b - a) * fy;
}

static float hmFbm(float x, float y, int seed, int octaves = 6) {
    float val = 0.0f, amp = 0.5f, freq = 1.0f;
    for (int i = 0; i < octaves; i++) {
        val += hmVnoise(x * freq, y * freq, seed + i * 1000) * amp;
        amp *= 0.5f;
        freq *= 2.0f;
    }
    return val;
}

HeightmapGenerator::HeightmapGenerator() : bounds{0,0,0,0} {
}

void HeightmapGenerator::setDepthAreas(const std::vector<DepthArea>& areas) {
    depthAreas = areas;
}

void HeightmapGenerator::setSoundings(const std::vector<Sounding>& s) {
    soundings = s;
}

void HeightmapGenerator::setCoastlines(const std::vector<CoastlineSegment>& c) {
    coastlines = c;
}

void HeightmapGenerator::setWaterHoles(const std::vector<WaterHole>& holes) {
    waterHoles = holes;
}

void HeightmapGenerator::setOSMWaterPolygons(const std::vector<std::vector<std::pair<double,double>>>& polys) {
    osmWaterPolygons = polys;
}

void HeightmapGenerator::setBarriers(const std::vector<std::vector<std::pair<double,double>>>& barriers) {
    barrierLines = barriers;
}

// ── GeoTIFF loading (shared by DEM and bathymetry) ─────────────────────────

bool HeightmapGenerator::loadGeoTIFF(const std::string& tifPath, DEMTile& tile) {
    GDALAllRegister();

    GDALDataset* ds = static_cast<GDALDataset*>(GDALOpen(tifPath.c_str(), GA_ReadOnly));
    if (!ds) {
        std::cerr << "HeightmapGenerator: Failed to open " << tifPath << std::endl;
        return false;
    }

    GDALRasterBand* band = ds->GetRasterBand(1);
    if (!band) {
        std::cerr << "HeightmapGenerator: No raster band in " << tifPath << std::endl;
        GDALClose(ds);
        return false;
    }

    tile.width = ds->GetRasterXSize();
    tile.height = ds->GetRasterYSize();

    double geoTransform[6];
    if (ds->GetGeoTransform(geoTransform) != CE_None) {
        std::cerr << "HeightmapGenerator: No geotransform in " << tifPath << std::endl;
        GDALClose(ds);
        return false;
    }

    tile.originLon = geoTransform[0];
    tile.pixelSizeLon = geoTransform[1];
    tile.originLat = geoTransform[3];
    tile.pixelSizeLat = geoTransform[5]; // Negative for north-up

    tile.data.resize(static_cast<size_t>(tile.width) * tile.height);
    CPLErr err = band->RasterIO(GF_Read, 0, 0, tile.width, tile.height,
                                 tile.data.data(), tile.width, tile.height,
                                 GDT_Float32, 0, 0);
    if (err != CE_None) {
        std::cerr << "HeightmapGenerator: Failed to read raster from " << tifPath << std::endl;
        GDALClose(ds);
        return false;
    }

    int hasNoData = 0;
    double nodata = band->GetNoDataValue(&hasNoData);
    if (hasNoData) {
        for (auto& v : tile.data) {
            if (v == static_cast<float>(nodata)) {
                v = std::numeric_limits<float>::quiet_NaN();
            }
        }
    }

    GDALClose(ds);
    return true;
}

float HeightmapGenerator::sampleTiles(const std::vector<DEMTile>& tiles,
                                       double lon, double lat) {
    for (const auto& tile : tiles) {
        double col = (lon - tile.originLon) / tile.pixelSizeLon;
        double row = (lat - tile.originLat) / tile.pixelSizeLat;

        int c = static_cast<int>(col);
        int r = static_cast<int>(row);

        if (c < 0 || c >= tile.width - 1 || r < 0 || r >= tile.height - 1)
            continue;

        float fCol = static_cast<float>(col - c);
        float fRow = static_cast<float>(row - r);

        float v00 = tile.data[r * tile.width + c];
        float v10 = tile.data[r * tile.width + (c + 1)];
        float v01 = tile.data[(r + 1) * tile.width + c];
        float v11 = tile.data[(r + 1) * tile.width + (c + 1)];

        if (std::isnan(v00) || std::isnan(v10) || std::isnan(v01) || std::isnan(v11)) {
            int nr = static_cast<int>(row + 0.5);
            int nc = static_cast<int>(col + 0.5);
            if (nr >= 0 && nr < tile.height && nc >= 0 && nc < tile.width) {
                float nearest = tile.data[nr * tile.width + nc];
                if (!std::isnan(nearest)) return nearest;
            }
            continue;
        }

        float top = v00 + (v10 - v00) * fCol;
        float bottom = v01 + (v11 - v01) * fCol;
        return top + (bottom - top) * fRow;
    }

    return std::numeric_limits<float>::quiet_NaN();
}

// ── DEM tile loading (land elevation) ──────────────────────────────────────

bool HeightmapGenerator::loadDEMTile(const std::string& tifPath) {
    DEMTile tile;
    if (!loadGeoTIFF(tifPath, tile)) return false;
    std::cout << "HeightmapGenerator: Loaded DEM tile " << tifPath
              << " (" << tile.width << "x" << tile.height << ")" << std::endl;
    demTiles.push_back(std::move(tile));
    return true;
}

bool HeightmapGenerator::loadDEMTiles(const std::vector<std::string>& tifPaths) {
    bool anyLoaded = false;
    for (const auto& path : tifPaths) {
        if (loadDEMTile(path)) anyLoaded = true;
    }
    return anyLoaded;
}

float HeightmapGenerator::sampleDEM(double lon, double lat) const {
    return sampleTiles(demTiles, lon, lat);
}

// ── Bathymetry tile loading (underwater depth - e.g. BlueTopo) ─────────────

bool HeightmapGenerator::loadBathymetryTile(const std::string& tifPath) {
    DEMTile tile;
    if (!loadGeoTIFF(tifPath, tile)) return false;
    std::cout << "HeightmapGenerator: Loaded bathymetry tile " << tifPath
              << " (" << tile.width << "x" << tile.height << ")" << std::endl;
    bathymetryTiles.push_back(std::move(tile));
    return true;
}

bool HeightmapGenerator::loadBathymetryTiles(const std::vector<std::string>& tifPaths) {
    bool anyLoaded = false;
    for (const auto& path : tifPaths) {
        if (loadBathymetryTile(path)) anyLoaded = true;
    }
    return anyLoaded;
}

float HeightmapGenerator::sampleBathymetry(double lon, double lat) const {
    return sampleTiles(bathymetryTiles, lon, lat);
}

void HeightmapGenerator::setBounds(const HeightmapBounds& b) {
    bounds = b;
    boundsSet = true;
}

HeightmapBounds HeightmapGenerator::computeBoundsFromData() const {
    HeightmapBounds b;
    b.minLon = std::numeric_limits<double>::max();
    b.maxLon = std::numeric_limits<double>::lowest();
    b.minLat = std::numeric_limits<double>::max();
    b.maxLat = std::numeric_limits<double>::lowest();

    auto expand = [&](double lon, double lat) {
        b.minLon = std::min(b.minLon, lon);
        b.maxLon = std::max(b.maxLon, lon);
        b.minLat = std::min(b.minLat, lat);
        b.maxLat = std::max(b.maxLat, lat);
    };

    for (const auto& area : depthAreas) {
        for (const auto& pt : area.boundary) {
            expand(pt.longitude, pt.latitude);
        }
    }

    for (const auto& s : soundings) {
        expand(s.longitude, s.latitude);
    }

    for (const auto& seg : coastlines) {
        for (const auto& pt : seg.points) {
            expand(pt.longitude, pt.latitude);
        }
    }

    // Add small margin (1% on each side)
    double lonMargin = (b.maxLon - b.minLon) * 0.01;
    double latMargin = (b.maxLat - b.minLat) * 0.01;
    if (lonMargin < 0.001) lonMargin = 0.001;
    if (latMargin < 0.001) latMargin = 0.001;
    b.minLon -= lonMargin;
    b.maxLon += lonMargin;
    b.minLat -= latMargin;
    b.maxLat += latMargin;

    return b;
}

// Ray casting algorithm for point-in-polygon
bool HeightmapGenerator::pointInPolygon(double x, double y,
                                         const std::vector<ChartPoint>& polygon) {
    if (polygon.size() < 3) return false;

    bool inside = false;
    size_t n = polygon.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        double xi = polygon[i].longitude, yi = polygon[i].latitude;
        double xj = polygon[j].longitude, yj = polygon[j].latitude;

        if (((yi > y) != (yj > y)) &&
            (x < (xj - xi) * (y - yi) / (yj - yi) + xi)) {
            inside = !inside;
        }
    }
    return inside;
}

float HeightmapGenerator::interpolateSoundings(double lon, double lat,
                                                const HeightmapParams& params) const {
    if (soundings.empty()) return std::numeric_limits<float>::quiet_NaN();

    // Collect distances and weights for IDW
    struct DistVal {
        double dist;
        double depth;
    };

    std::vector<DistVal> candidates;
    candidates.reserve(soundings.size());

    // Approximate metres per degree at this latitude
    double cosLat = cos(lat * M_PI / 180.0);
    double mPerDegLat = 111320.0;
    double mPerDegLon = 111320.0 * cosLat;

    for (const auto& s : soundings) {
        double dx = (s.longitude - lon) * mPerDegLon;
        double dy = (s.latitude - lat) * mPerDegLat;
        double dist = sqrt(dx * dx + dy * dy);
        if (dist < 0.1) {
            // Very close to an exact sounding - use it directly
            return static_cast<float>(-s.depth); // Negative because depth is positive down
        }
        candidates.push_back({dist, s.depth});
    }

    // Sort by distance and take nearest N
    std::sort(candidates.begin(), candidates.end(),
              [](const DistVal& a, const DistVal& b) { return a.dist < b.dist; });

    int count = std::min(params.idwNeighbours, static_cast<int>(candidates.size()));
    if (count == 0) return std::numeric_limits<float>::quiet_NaN();

    double weightSum = 0;
    double valueSum = 0;
    for (int i = 0; i < count; i++) {
        double w = 1.0 / pow(candidates[i].dist, params.idwPower);
        weightSum += w;
        valueSum += w * candidates[i].depth;
    }

    if (weightSum == 0) return std::numeric_limits<float>::quiet_NaN();
    return static_cast<float>(-(valueSum / weightSum)); // Negative = below water
}

bool HeightmapGenerator::isLand(double lon, double lat) const {
    // Step 1: Check OSM water polygons first (highest authority for inland water).
    // If the point is inside an OSM water polygon, it is definitively water.
    for (const auto& poly : osmWaterPolygons) {
        if (poly.size() < 3) continue;
        // OSM polygons are stored as (lat, lon) pairs; convert for ray-cast
        bool inside = false;
        size_t n = poly.size();
        for (size_t i = 0, j = n - 1; i < n; j = i++) {
            double xi = poly[i].second, yi = poly[i].first;  // lon, lat
            double xj = poly[j].second, yj = poly[j].first;
            if (((yi > lat) != (yj > lat)) &&
                (lon < (xj - xi) * (lat - yi) / (yj - yi) + xi)) {
                inside = !inside;
            }
        }
        if (inside) return false; // Inside a water polygon -> not land
    }

    // Step 2: Check LNDARE inner rings (water holes within land).
    // If the point is inside a water hole, it is water even if the outer
    // LNDARE polygon says land.
    for (const auto& hole : waterHoles) {
        if (hole.boundary.size() >= 3 && pointInPolygon(lon, lat, hole.boundary)) {
            return false; // Inside a water hole -> not land
        }
    }

    // Step 3: Check LNDARE/COALNE closed polygons (chart authority for land).
    for (const auto& seg : coastlines) {
        if (seg.points.size() >= 3) {
            const auto& first = seg.points.front();
            const auto& last = seg.points.back();
            double closeDist = fabs(first.longitude - last.longitude) +
                               fabs(first.latitude - last.latitude);
            if (closeDist < 0.0001) {
                if (pointInPolygon(lon, lat, seg.points)) {
                    return true;
                }
            }
        }
    }
    return false;
}

std::vector<std::vector<float>> HeightmapGenerator::generate(
    const HeightmapParams& params) const {

    HeightmapBounds b = boundsSet ? bounds : computeBoundsFromData();

    int res = params.resolution;
    // Initialize grid with NaN (unset)
    std::vector<std::vector<float>> grid(res,
        std::vector<float>(res, std::numeric_limits<float>::quiet_NaN()));

    double lonStep = (b.maxLon - b.minLon) / (res - 1);
    double latStep = (b.maxLat - b.minLat) / (res - 1);

    maxHeight = 0;
    maxDepth = 0;

    // Pass 1: Rasterize depth area polygons
    for (int row = 0; row < res; row++) {
        double lat = b.maxLat - row * latStep; // Top to bottom
        for (int col = 0; col < res; col++) {
            double lon = b.minLon + col * lonStep;

            // Check each depth area (last match wins - deeper areas typically overlay)
            for (const auto& area : depthAreas) {
                if (area.boundary.size() >= 3 &&
                    pointInPolygon(lon, lat, area.boundary)) {
                    // Use average of min/max depth, negative = below water
                    float depth = -static_cast<float>((area.minDepth + area.maxDepth) / 2.0);
                    grid[row][col] = depth;
                }
            }
        }
    }

    // Pass 2: Refine with sounding interpolation where available
    if (!soundings.empty()) {
        for (int row = 0; row < res; row++) {
            double lat = b.maxLat - row * latStep;
            for (int col = 0; col < res; col++) {
                double lon = b.minLon + col * lonStep;

                // Only interpolate in water areas (already set from depth areas or NaN)
                if (grid[row][col] <= 0 || std::isnan(grid[row][col])) {
                    float interpolated = interpolateSoundings(lon, lat, params);
                    if (!std::isnan(interpolated)) {
                        // Blend: if depth area value exists, average with interpolated
                        if (!std::isnan(grid[row][col]) && grid[row][col] < 0) {
                            grid[row][col] = (grid[row][col] + interpolated) / 2.0f;
                        } else if (std::isnan(grid[row][col])) {
                            grid[row][col] = interpolated;
                        }
                    }
                }
            }
        }
    }

    // Pass 2b: Override with high-resolution bathymetry (BlueTopo) where available
    if (!bathymetryTiles.empty()) {
        for (int row = 0; row < res; row++) {
            double lat = b.maxLat - row * latStep;
            for (int col = 0; col < res; col++) {
                double lon = b.minLon + col * lonStep;

                float bathyVal = sampleBathymetry(lon, lat);
                if (!std::isnan(bathyVal) && bathyVal < 0.0f) {
                    // BlueTopo provides negative values for underwater,
                    // which matches our convention. Override chart data.
                    grid[row][col] = bathyVal;
                }
            }
        }
    }

    // Pass 3: Set land areas (using DEM if available, otherwise defaultLandHeight)
    bool haveDEM = !demTiles.empty();
    for (int row = 0; row < res; row++) {
        double lat = b.maxLat - row * latStep;
        for (int col = 0; col < res; col++) {
            double lon = b.minLon + col * lonStep;

            if (isLand(lon, lat)) {
                if (haveDEM) {
                    // Domain warping: warp DEM sampling coordinates with FBM
                    // for organic terrain distortion (~7m displacement)
                    float warpX = (hmFbm((float)(lon * 2000.0), (float)(lat * 2000.0), 12345, 4) - 0.5f) * 0.00006f;
                    float warpY = (hmFbm((float)(lon * 2000.0), (float)(lat * 2000.0), 54321, 4) - 0.5f) * 0.00006f;
                    float demHeight = sampleDEM(lon + warpX, lat + warpY);
                    if (!std::isnan(demHeight) && demHeight >= 0.0f) {
                        grid[row][col] = demHeight;
                    } else {
                        grid[row][col] = static_cast<float>(params.defaultLandHeight);
                    }
                } else {
                    grid[row][col] = static_cast<float>(params.defaultLandHeight);
                }
            }

            // Fill remaining NaN with default sea depth
            if (std::isnan(grid[row][col])) {
                grid[row][col] = -static_cast<float>(params.defaultSeaDepth);
            }

            // Track stats
            if (grid[row][col] > maxHeight) maxHeight = grid[row][col];
            if (grid[row][col] < -maxDepth) maxDepth = -grid[row][col];
        }
    }

    // Pass 4: Barrier flood-fill (barrages, dams, breakwaters)
    if (!barrierLines.empty()) {
        applyBarrierFloodFill(grid, b);
    }

    // Pass 4b: Gentle terrain undulation for inland areas only.
    // Skips low-elevation coastal land (<5m) to avoid destroying beaches and thin features.
    // Amplitude scales quadratically with elevation so hills get more variation than flats.
    {
        for (int row = 0; row < res; row++) {
            double lat = b.maxLat - row * latStep;
            for (int col = 0; col < res; col++) {
                float baseH = grid[row][col];
                if (baseH <= 5.0f) continue; // Skip water + low coastal land entirely

                double lon = b.minLon + col * lonStep;

                // Large-scale rolling undulation (~300-500m wavelength)
                float roll = hmFbm((float)(lon * 1200.0), (float)(lat * 1200.0), 33333, 4);
                roll = (roll - 0.5f) * 2.0f; // [-1, 1]

                // Amplitude: quadratic ramp from 5m elevation, caps at 4m perturbation
                float above5 = baseH - 5.0f;
                float amplitude = std::min(above5 * above5 * 0.008f, 4.0f);

                grid[row][col] += roll * amplitude;
                if (grid[row][col] < 0.5f) grid[row][col] = 0.5f;
            }
        }
        std::cout << "HeightmapGenerator: Terrain undulation applied" << std::endl;
    }

    // Pass 5: Coastal fractal noise
    // Adds irregular coves, rocky points, and natural beach edges
    // by perturbing heights within ~200m of the coastline.
    {
        // BFS distance field from coastline boundary
        std::vector<int> coastDist(res * res, 999);
        std::queue<int> bfsQ;

        for (int row = 1; row < res - 1; row++) {
            for (int col = 1; col < res - 1; col++) {
                int idx = row * res + col;
                bool isLandHere = grid[row][col] > 0.0f;
                bool onBoundary = false;
                for (int dy = -1; dy <= 1 && !onBoundary; dy++) {
                    for (int dx = -1; dx <= 1 && !onBoundary; dx++) {
                        if (dy == 0 && dx == 0) continue;
                        int nr = row + dy, nc = col + dx;
                        if (nr >= 0 && nr < res && nc >= 0 && nc < res) {
                            if ((grid[nr][nc] > 0.0f) != isLandHere) onBoundary = true;
                        }
                    }
                }
                if (onBoundary) { coastDist[idx] = 0; bfsQ.push(idx); }
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
                    if (coastDist[ni] > coastDist[idx] + 1) {
                        coastDist[ni] = coastDist[idx] + 1;
                        bfsQ.push(ni);
                    }
                }
            }
        }

        const int maxDistPx = 25; // ~250m at 10m/pixel
        for (int row = 0; row < res; row++) {
            double lat = b.maxLat - row * latStep;
            for (int col = 0; col < res; col++) {
                double lon = b.minLon + col * lonStep;
                int d = coastDist[row * res + col];
                if (d >= maxDistPx) continue;

                float influence = 1.0f - (float)d / (float)maxDistPx;
                influence *= influence; // Quadratic falloff

                // Multi-octave FBM at geographic scale for natural fractal coastline
                float noise = hmFbm((float)(lon * 5000.0), (float)(lat * 5000.0), 777777, 6);
                noise = (noise - 0.5f) * 2.0f; // Range [-1, 1]

                grid[row][col] += noise * influence * 1.5f; // +/- 1.5m max
            }
        }

        std::cout << "HeightmapGenerator: Coastal fractal noise + dunes applied" << std::endl;
    }

    // Pass 6: Thermal erosion
    // Material slides from steep slopes to adjacent lower cells.
    // Creates gullies, sediment fans, and smoothed beach profiles.
    {
        const int iterations = 80;
        const float talusAngle = 0.8f; // slope threshold (m/pixel)
        const float transferRate = 0.3f;

        std::vector<std::vector<float>> delta(res, std::vector<float>(res, 0.0f));

        for (int iter = 0; iter < iterations; iter++) {
            for (auto& row : delta) std::fill(row.begin(), row.end(), 0.0f);

            for (int row = 1; row < res - 1; row++) {
                for (int col = 1; col < res - 1; col++) {
                    if (grid[row][col] <= 0.0f) continue; // Skip water

                    float maxDiff = 0.0f;
                    int bestDy = 0, bestDx = 0;
                    for (int dy = -1; dy <= 1; dy++) {
                        for (int dx = -1; dx <= 1; dx++) {
                            if (dy == 0 && dx == 0) continue;
                            float diff = grid[row][col] - grid[row + dy][col + dx];
                            if (diff > maxDiff) {
                                maxDiff = diff;
                                bestDy = dy;
                                bestDx = dx;
                            }
                        }
                    }

                    if (maxDiff > talusAngle) {
                        float transfer = (maxDiff - talusAngle) * transferRate;
                        delta[row][col] -= transfer;
                        delta[row + bestDy][col + bestDx] += transfer;
                    }
                }
            }

            for (int row = 1; row < res - 1; row++) {
                for (int col = 1; col < res - 1; col++) {
                    grid[row][col] += delta[row][col];
                }
            }
        }

        // Recompute height stats after erosion
        maxHeight = 0;
        maxDepth = 0;
        for (int row = 0; row < res; row++) {
            for (int col = 0; col < res; col++) {
                if (grid[row][col] > maxHeight) maxHeight = grid[row][col];
                if (grid[row][col] < -maxDepth) maxDepth = -grid[row][col];
            }
        }

        std::cout << "HeightmapGenerator: Thermal erosion applied (" << iterations << " iterations)" << std::endl;
    }

    return grid;
}

void HeightmapGenerator::applyBarrierFloodFill(
    std::vector<std::vector<float>>& grid,
    const HeightmapBounds& b) const {

    int res = static_cast<int>(grid.size());
    if (res == 0) return;

    // Flatten 2D grid to flat row-major for the standalone algorithm
    std::vector<float> flat(res * res);
    for (int py = 0; py < res; py++) {
        for (int px = 0; px < res; px++) {
            flat[py * res + px] = grid[py][px];
        }
    }

    BarrierFloodFill::Bounds bff{b.minLon, b.maxLon, b.minLat, b.maxLat};
    BarrierFloodFill::apply(flat.data(), res, bff, barrierLines);

    // Copy results back to 2D grid
    for (int py = 0; py < res; py++) {
        for (int px = 0; px < res; px++) {
            grid[py][px] = flat[py * res + px];
        }
    }
}

std::vector<uint8_t> HeightmapGenerator::encodeRGB(
    const std::vector<std::vector<float>>& heightGrid) {

    if (heightGrid.empty()) return {};

    int rows = static_cast<int>(heightGrid.size());
    int cols = static_cast<int>(heightGrid[0].size());
    std::vector<uint8_t> rgb(rows * cols * 3);

    for (int row = 0; row < rows; row++) {
        for (int col = 0; col < cols; col++) {
            // Bridge Command format: Height = R*256 + G + B/256 - 32768
            // So: encoded = Height + 32768
            // R = encoded / 256 (integer part)
            // G = encoded % 256 (integer remainder)
            // B = fractional * 256
            float height = heightGrid[row][col];
            double encoded = static_cast<double>(height) + 32768.0;

            // Clamp to valid range (0 to 65535.996)
            encoded = std::max(0.0, std::min(encoded, 65535.996));

            int intPart = static_cast<int>(encoded);
            double frac = encoded - intPart;

            uint8_t r = static_cast<uint8_t>(intPart / 256);
            uint8_t g = static_cast<uint8_t>(intPart % 256);
            uint8_t b = static_cast<uint8_t>(frac * 256.0);

            int idx = (row * cols + col) * 3;
            rgb[idx + 0] = r;
            rgb[idx + 1] = g;
            rgb[idx + 2] = b;
        }
    }

    return rgb;
}

bool HeightmapGenerator::writePNG(const std::string& path,
                                   const std::vector<uint8_t>& rgbData,
                                   int width, int height) {
    if (rgbData.size() != static_cast<size_t>(width * height * 3)) {
        std::cerr << "HeightmapGenerator: RGB data size mismatch" << std::endl;
        return false;
    }

    int result = stbi_write_png(path.c_str(), width, height, 3,
                                rgbData.data(), width * 3);
    return result != 0;
}

bool HeightmapGenerator::generateAndWrite(const std::string& outputPath,
                                           const HeightmapParams& params) const {
    auto grid = generate(params);
    if (grid.empty()) return false;

    auto rgb = encodeRGB(grid);
    return writePNG(outputPath, rgb, params.resolution, params.resolution);
}

#endif // WITH_GDAL
