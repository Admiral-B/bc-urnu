#include "ElevationTile.hpp"
#include "TileDownloader.hpp"
#include "TileMath.hpp"

#include <cmath>
#include <algorithm>
#include <chrono>
#include <thread>
#include <iostream>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4996)
#endif

#include "../libs/stb/stb_image.h"

#ifdef _MSC_VER
#pragma warning(pop)
#endif

static const char* TERRARIUM_URL =
    "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png";

int ElevationTile::suggestZoom(double minLat, double maxLat,
                                double minLon, double maxLon,
                                int targetSize) {
    double lonExtent = maxLon - minLon;
    double latExtent = maxLat - minLat;
    double maxExtent = std::max(lonExtent, latExtent);

    if (maxExtent <= 0) return 10;

    double z = std::log2(targetSize * 360.0 / (maxExtent * 256.0));
    int zoom = static_cast<int>(std::round(z));
    // Cap at 12: elevation data beyond ~40m/pixel adds download cost
    // without meaningful detail for ship simulator heightmaps.
    return std::max(1, std::min(zoom, 12));
}

std::vector<float> ElevationTile::generate(
    double minLat, double maxLat, double minLon, double maxLon,
    int zoom, int resolution,
    const std::string& cacheDir,
    ProgressCallback progress) {

    // Calculate tile grid covering the bounding box
    int minTileX = TileMath::lonToTileX(minLon, zoom);
    int maxTileX = TileMath::lonToTileX(maxLon, zoom);
    int minTileY = TileMath::latToTileY(maxLat, zoom); // Y increases southward
    int maxTileY = TileMath::latToTileY(minLat, zoom);

    // Clamp to valid tile range
    int maxTileIdx = (1 << zoom) - 1;
    minTileX = std::max(0, minTileX);
    maxTileX = std::min(maxTileIdx, maxTileX);
    minTileY = std::max(0, minTileY);
    maxTileY = std::min(maxTileIdx, maxTileY);

    int tilesX = maxTileX - minTileX + 1;
    int tilesY = maxTileY - minTileY + 1;
    int totalTiles = tilesX * tilesY;

    if (totalTiles <= 0 || totalTiles > 1024) {
        std::cerr << "ElevationTile: tile count out of range (" << totalTiles << ")" << std::endl;
        return {};
    }

    // Create downloader for AWS Terrain Tiles
    std::string elevCacheDir = cacheDir + "/elevation/";
    TileDownloader downloader(TERRARIUM_URL, elevCacheDir);
    downloader.setUserAgent("BridgeCommand/6.0 (world-generator)");

    // Queue all tile downloads
    for (int ty = minTileY; ty <= maxTileY; ty++) {
        for (int tx = minTileX; tx <= maxTileX; tx++) {
            downloader.getTile(zoom, tx, ty);
        }
    }

    // Wait for all tiles (poll with timeout)
    int tilesReady = 0;
    auto startTime = std::chrono::steady_clock::now();
    const int timeoutSec = 120;

    while (tilesReady < totalTiles) {
        auto elapsed = std::chrono::steady_clock::now() - startTime;
        if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() > timeoutSec) {
            std::cerr << "ElevationTile: timeout waiting for tile downloads ("
                      << tilesReady << "/" << totalTiles << ")" << std::endl;
            break;
        }

        tilesReady = 0;
        for (int ty = minTileY; ty <= maxTileY; ty++) {
            for (int tx = minTileX; tx <= maxTileX; tx++) {
                if (downloader.isTileReady(zoom, tx, ty)) {
                    tilesReady++;
                }
            }
        }

        if (progress && !progress(tilesReady, totalTiles)) {
            return {}; // Cancelled
        }

        if (tilesReady < totalTiles) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }

    // Composite: decode each tile's RGB into a float elevation grid
    const int TILE_SIZE = 256;
    int compositeW = tilesX * TILE_SIZE;
    int compositeH = tilesY * TILE_SIZE;

    // Float elevation composite (NaN = no data)
    std::vector<float> composite(compositeW * compositeH,
                                  std::numeric_limits<float>::quiet_NaN());

    int placed = 0;
    for (int ty = minTileY; ty <= maxTileY; ty++) {
        for (int tx = minTileX; tx <= maxTileX; tx++) {
            std::vector<uint8_t> pngData = downloader.getTile(zoom, tx, ty);
            if (pngData.empty()) continue;

            int w, h, channels;
            unsigned char* pixels = stbi_load_from_memory(
                pngData.data(), static_cast<int>(pngData.size()),
                &w, &h, &channels, 3); // Force RGB

            if (!pixels) continue;

            int offX = (tx - minTileX) * TILE_SIZE;
            int offY = (ty - minTileY) * TILE_SIZE;

            int copyW = std::min(w, TILE_SIZE);
            int copyH = std::min(h, TILE_SIZE);
            for (int row = 0; row < copyH && (offY + row) < compositeH; row++) {
                for (int col = 0; col < copyW && (offX + col) < compositeW; col++) {
                    int srcIdx = (row * w + col) * 3;
                    uint8_t r = pixels[srcIdx + 0];
                    uint8_t g = pixels[srcIdx + 1];
                    uint8_t b = pixels[srcIdx + 2];

                    // Terrarium encoding: elevation = R*256 + G + B/256 - 32768
                    float elevation = r * 256.0f + g + b / 256.0f - 32768.0f;
                    composite[(offY + row) * compositeW + (offX + col)] = elevation;
                }
            }

            stbi_image_free(pixels);
            placed++;
        }
    }

    if (placed == 0) {
        std::cerr << "ElevationTile: no tiles were decoded" << std::endl;
        return {};
    }

    // Calculate pixel coordinates of bounding box within composite
    double globalMinPixelX = TileMath::lonToPixelX(minLon, zoom, TILE_SIZE);
    double globalMaxPixelX = TileMath::lonToPixelX(maxLon, zoom, TILE_SIZE);
    double globalMinPixelY = TileMath::latToPixelY(maxLat, zoom, TILE_SIZE); // maxLat = north = lower Y
    double globalMaxPixelY = TileMath::latToPixelY(minLat, zoom, TILE_SIZE);

    double tileGridOriginX = static_cast<double>(minTileX) * TILE_SIZE;
    double tileGridOriginY = static_cast<double>(minTileY) * TILE_SIZE;

    double cropLeft = globalMinPixelX - tileGridOriginX;
    double cropTop = globalMinPixelY - tileGridOriginY;
    double cropRight = globalMaxPixelX - tileGridOriginX;
    double cropBottom = globalMaxPixelY - tileGridOriginY;

    // Clamp crop bounds
    cropLeft = std::max(0.0, std::min(cropLeft, (double)compositeW));
    cropTop = std::max(0.0, std::min(cropTop, (double)compositeH));
    cropRight = std::max(cropLeft + 1.0, std::min(cropRight, (double)compositeW));
    cropBottom = std::max(cropTop + 1.0, std::min(cropBottom, (double)compositeH));

    double cropW = cropRight - cropLeft;
    double cropH = cropBottom - cropTop;

    // Bilinear resample from composite to target resolution
    std::vector<float> result(resolution * resolution);

    auto sampleElev = [&](int x, int y) -> float {
        x = std::max(0, std::min(x, compositeW - 1));
        y = std::max(0, std::min(y, compositeH - 1));
        return composite[y * compositeW + x];
    };

    for (int oy = 0; oy < resolution; oy++) {
        double srcY = cropTop + (oy + 0.5) * cropH / resolution;
        for (int ox = 0; ox < resolution; ox++) {
            double srcX = cropLeft + (ox + 0.5) * cropW / resolution;

            int sx = static_cast<int>(srcX);
            int sy = static_cast<int>(srcY);
            double fx = srcX - sx;
            double fy = srcY - sy;

            sx = std::max(0, std::min(sx, compositeW - 2));
            sy = std::max(0, std::min(sy, compositeH - 2));

            float v00 = sampleElev(sx, sy);
            float v10 = sampleElev(sx + 1, sy);
            float v01 = sampleElev(sx, sy + 1);
            float v11 = sampleElev(sx + 1, sy + 1);

            // If any corner is NaN, use nearest valid neighbor
            if (std::isnan(v00) || std::isnan(v10) || std::isnan(v01) || std::isnan(v11)) {
                // Find closest non-NaN value
                float best = std::numeric_limits<float>::quiet_NaN();
                if (!std::isnan(v00)) best = v00;
                else if (!std::isnan(v10)) best = v10;
                else if (!std::isnan(v01)) best = v01;
                else if (!std::isnan(v11)) best = v11;
                result[oy * resolution + ox] = best;
            } else {
                float v = static_cast<float>(
                    v00 * (1.0 - fx) * (1.0 - fy)
                  + v10 * fx * (1.0 - fy)
                  + v01 * (1.0 - fx) * fy
                  + v11 * fx * fy);
                result[oy * resolution + ox] = v;
            }
        }
    }

    return result;
}
