#include "SatelliteTexture.hpp"
#include "TileDownloader.hpp"
#include "TileMath.hpp"

#include <cmath>
#include <algorithm>
#include <chrono>
#include <thread>
#include <iostream>

// Suppress MSVC deprecation warnings in stb headers
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4996)
#endif

// stb_image for decoding PNG tile data (implementation is in TileTextureManager.cpp)
#include "../libs/stb/stb_image.h"

// stb_image_write for writing output PNG (implementation is in HeightmapGenerator.cpp)
#include "../libs/stb/stb_image_write.h"

#ifdef _MSC_VER
#pragma warning(pop)
#endif

static const char* ESRI_URL =
    "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}";

int SatelliteTexture::suggestZoom(double minLat, double maxLat,
                                   double minLon, double maxLon,
                                   int targetSize) {
    double lonExtent = maxLon - minLon;
    double latExtent = maxLat - minLat;
    double maxExtent = std::max(lonExtent, latExtent);

    // At zoom z, there are 256 * 2^z pixels across 360 degrees of longitude.
    // We want: pixelsNeeded ~= targetSize
    // pixelsNeeded = (lonExtent / 360) * 256 * 2^z
    // Solving for z: 2^z = targetSize * 360 / (lonExtent * 256)
    // z = log2(targetSize * 360 / (maxExtent * 256))

    if (maxExtent <= 0) return 14;

    double z = std::log2(targetSize * 360.0 / (maxExtent * 256.0));
    int zoom = static_cast<int>(std::round(z));
    return std::max(1, std::min(zoom, 18));
}

std::vector<uint8_t> SatelliteTexture::generate(
    double minLat, double maxLat, double minLon, double maxLon,
    int zoom, int targetSize,
    const std::string& cacheDir,
    int& outputWidth, int& outputHeight,
    ProgressCallback progress) {

    outputWidth = 0;
    outputHeight = 0;

    // Calculate tile grid covering the bounding box
    int minTileX = TileMath::lonToTileX(minLon, zoom);
    int maxTileX = TileMath::lonToTileX(maxLon, zoom);
    int minTileY = TileMath::latToTileY(maxLat, zoom); // Note: Y increases southward
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
        std::cerr << "SatelliteTexture: tile count out of range (" << totalTiles << ")" << std::endl;
        return {};
    }

    // Create a TileDownloader for ESRI satellite tiles
    std::string satCacheDir = cacheDir + "/satellite/";
    TileDownloader downloader(ESRI_URL, satCacheDir);
    downloader.setUserAgent("BridgeCommand/6.0 (world-generator)");

    // Request all tiles (triggers async downloads)
    for (int ty = minTileY; ty <= maxTileY; ty++) {
        for (int tx = minTileX; tx <= maxTileX; tx++) {
            downloader.getTile(zoom, tx, ty);
        }
    }

    // Wait for all tiles to download (poll with timeout)
    int tilesReady = 0;
    auto startTime = std::chrono::steady_clock::now();
    const int timeoutSec = 120; // 2 minute timeout

    while (tilesReady < totalTiles) {
        auto elapsed = std::chrono::steady_clock::now() - startTime;
        if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() > timeoutSec) {
            std::cerr << "SatelliteTexture: timeout waiting for tile downloads" << std::endl;
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

    // Composite image: each tile is 256x256 pixels
    const int TILE_SIZE = 256;
    int compositeW = tilesX * TILE_SIZE;
    int compositeH = tilesY * TILE_SIZE;

    // RGB buffer for the composite
    std::vector<uint8_t> composite(compositeW * compositeH * 3, 128);

    // Decode and place each tile
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

            // Calculate position in composite
            int offX = (tx - minTileX) * TILE_SIZE;
            int offY = (ty - minTileY) * TILE_SIZE;

            // Copy tile pixels into composite
            int copyW = std::min(w, TILE_SIZE);
            int copyH = std::min(h, TILE_SIZE);
            for (int row = 0; row < copyH && (offY + row) < compositeH; row++) {
                for (int col = 0; col < copyW && (offX + col) < compositeW; col++) {
                    int srcIdx = (row * w + col) * 3;
                    int dstIdx = ((offY + row) * compositeW + (offX + col)) * 3;
                    composite[dstIdx + 0] = pixels[srcIdx + 0];
                    composite[dstIdx + 1] = pixels[srcIdx + 1];
                    composite[dstIdx + 2] = pixels[srcIdx + 2];
                }
            }

            stbi_image_free(pixels);
            placed++;
        }
    }

    if (placed == 0) {
        std::cerr << "SatelliteTexture: no tiles were decoded" << std::endl;
        return {};
    }

    // Now we have a composite that covers the full tile grid.
    // We need to crop to the exact bounding box and resample to targetSize.

    // Calculate pixel coordinates of the bounding box within the composite
    double globalMinPixelX = TileMath::lonToPixelX(minLon, zoom, TILE_SIZE);
    double globalMaxPixelX = TileMath::lonToPixelX(maxLon, zoom, TILE_SIZE);
    double globalMinPixelY = TileMath::latToPixelY(maxLat, zoom, TILE_SIZE); // maxLat = north = lower Y
    double globalMaxPixelY = TileMath::latToPixelY(minLat, zoom, TILE_SIZE); // minLat = south = higher Y

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

    // If targetSize is specified, resample; otherwise use cropped dimensions
    int outW, outH;
    if (targetSize > 0) {
        outW = targetSize;
        outH = targetSize;
    } else {
        outW = static_cast<int>(cropW);
        outH = static_cast<int>(cropH);
    }

    // Bilinear resample from composite (crop region) to output
    std::vector<uint8_t> result(outW * outH * 3);

    for (int oy = 0; oy < outH; oy++) {
        double srcY = cropTop + (oy + 0.5) * cropH / outH;
        for (int ox = 0; ox < outW; ox++) {
            double srcX = cropLeft + (ox + 0.5) * cropW / outW;

            // Bilinear interpolation
            int sx = static_cast<int>(srcX);
            int sy = static_cast<int>(srcY);
            double fx = srcX - sx;
            double fy = srcY - sy;

            sx = std::max(0, std::min(sx, compositeW - 2));
            sy = std::max(0, std::min(sy, compositeH - 2));

            auto sample = [&](int x, int y, int c) -> double {
                x = std::max(0, std::min(x, compositeW - 1));
                y = std::max(0, std::min(y, compositeH - 1));
                return composite[(y * compositeW + x) * 3 + c];
            };

            int dstIdx = (oy * outW + ox) * 3;
            for (int c = 0; c < 3; c++) {
                double v = sample(sx, sy, c) * (1 - fx) * (1 - fy)
                         + sample(sx + 1, sy, c) * fx * (1 - fy)
                         + sample(sx, sy + 1, c) * (1 - fx) * fy
                         + sample(sx + 1, sy + 1, c) * fx * fy;
                result[dstIdx + c] = static_cast<uint8_t>(std::max(0.0, std::min(255.0, v)));
            }
        }
    }

    outputWidth = outW;
    outputHeight = outH;
    return result;
}

bool SatelliteTexture::writePNG(const std::string& path,
                                 const std::vector<uint8_t>& rgbData,
                                 int width, int height) {
    if (rgbData.size() != static_cast<size_t>(width * height * 3)) {
        return false;
    }
    int result = stbi_write_png(path.c_str(), width, height, 3,
                                rgbData.data(), width * 3);
    return result != 0;
}
