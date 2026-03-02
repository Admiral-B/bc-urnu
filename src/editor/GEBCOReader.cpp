#include "GEBCOReader.hpp"
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <limits>

#ifdef WITH_GDAL
#include "gdal_priv.h"
#endif

namespace fs = std::filesystem;

bool GEBCOReader::loadBinaryGrid(const std::string& binPath) {
    std::ifstream file(binPath, std::ios::binary);
    if (!file) return false;

    DepthTile tile;

    // Read header: bounding box (4 doubles) + dimensions (2 ints)
    file.read(reinterpret_cast<char*>(&tile.minLon), sizeof(double));
    file.read(reinterpret_cast<char*>(&tile.maxLon), sizeof(double));
    file.read(reinterpret_cast<char*>(&tile.minLat), sizeof(double));
    file.read(reinterpret_cast<char*>(&tile.maxLat), sizeof(double));
    file.read(reinterpret_cast<char*>(&tile.width), sizeof(int32_t));
    file.read(reinterpret_cast<char*>(&tile.height), sizeof(int32_t));

    if (!file || tile.width <= 0 || tile.height <= 0 ||
        tile.width > 100000 || tile.height > 100000) return false;

    tile.pixelSizeLon = (tile.maxLon - tile.minLon) / tile.width;
    tile.pixelSizeLat = (tile.maxLat - tile.minLat) / tile.height;

    // Read depth data
    size_t dataSize = static_cast<size_t>(tile.width) * tile.height;
    tile.data.resize(dataSize);
    file.read(reinterpret_cast<char*>(tile.data.data()), dataSize * sizeof(int16_t));

    if (!file) return false;

    tiles.push_back(std::move(tile));
    return true;
}

bool GEBCOReader::loadBinaryDirectory(const std::string& dirPath) {
    bool anyLoaded = false;
    try {
        for (const auto& entry : fs::directory_iterator(dirPath)) {
            if (entry.path().extension() == ".bin" &&
                entry.path().string().find(".gebco.") != std::string::npos) {
                if (loadBinaryGrid(entry.path().string())) {
                    anyLoaded = true;
                }
            }
        }
    } catch (...) {}
    return anyLoaded;
}

#ifdef WITH_GDAL
bool GEBCOReader::loadGeoTIFF(const std::string& tifPath) {
    GDALAllRegister();

    GDALDataset* ds = static_cast<GDALDataset*>(GDALOpen(tifPath.c_str(), GA_ReadOnly));
    if (!ds) return false;

    GDALRasterBand* band = ds->GetRasterBand(1);
    if (!band) {
        GDALClose(ds);
        return false;
    }

    double geoTransform[6];
    if (ds->GetGeoTransform(geoTransform) != CE_None) {
        GDALClose(ds);
        return false;
    }

    DepthTile tile;
    tile.width = ds->GetRasterXSize();
    tile.height = ds->GetRasterYSize();
    tile.minLon = geoTransform[0];
    tile.pixelSizeLon = geoTransform[1];
    tile.minLat = geoTransform[3] + geoTransform[5] * tile.height; // Bottom edge
    tile.maxLat = geoTransform[3]; // Top edge (origin)
    tile.maxLon = tile.minLon + tile.pixelSizeLon * tile.width;
    tile.pixelSizeLat = -geoTransform[5]; // Make positive

    tile.data.resize(static_cast<size_t>(tile.width) * tile.height);
    CPLErr err = band->RasterIO(GF_Read, 0, 0, tile.width, tile.height,
                                 tile.data.data(), tile.width, tile.height,
                                 GDT_Int16, 0, 0);
    GDALClose(ds);

    if (err != CE_None) return false;

    tiles.push_back(std::move(tile));
    return true;
}

bool GEBCOReader::loadGeoTIFFDirectory(const std::string& dirPath) {
    bool anyLoaded = false;
    try {
        for (const auto& entry : fs::directory_iterator(dirPath)) {
            auto ext = entry.path().extension().string();
            if (ext == ".tif" || ext == ".tiff") {
                if (loadGeoTIFF(entry.path().string())) {
                    anyLoaded = true;
                }
            }
        }
    } catch (...) {}
    return anyLoaded;
}
#endif // WITH_GDAL

const GEBCOReader::DepthTile* GEBCOReader::findTile(double lat, double lon) const {
    for (const auto& tile : tiles) {
        if (lon >= tile.minLon && lon <= tile.maxLon &&
            lat >= tile.minLat && lat <= tile.maxLat) {
            return &tile;
        }
    }
    return nullptr;
}

float GEBCOReader::sampleTile(const DepthTile& tile, double lat, double lon) const {
    // Convert lat/lon to pixel coordinates
    // Data is stored top-to-bottom (row 0 = max lat)
    double px = (lon - tile.minLon) / tile.pixelSizeLon;
    double py = (tile.maxLat - lat) / tile.pixelSizeLat;

    int ix = static_cast<int>(px);
    int iy = static_cast<int>(py);

    // Clamp to valid range
    ix = std::max(0, std::min(ix, tile.width - 1));
    iy = std::max(0, std::min(iy, tile.height - 1));

    return static_cast<float>(tile.data[static_cast<size_t>(iy) * tile.width + ix]);
}

float GEBCOReader::getDepth(double lat, double lon) const {
    const DepthTile* tile = findTile(lat, lon);
    if (!tile) return std::numeric_limits<float>::quiet_NaN();
    return sampleTile(*tile, lat, lon);
}

std::vector<std::vector<float>> GEBCOReader::getDepthGrid(
    double minLat, double maxLat, double minLon, double maxLon,
    int rows, int cols) const
{
    std::vector<std::vector<float>> grid(rows, std::vector<float>(cols, 0.0f));

    double latStep = (maxLat - minLat) / (rows - 1);
    double lonStep = (maxLon - minLon) / (cols - 1);

    for (int r = 0; r < rows; r++) {
        double lat = maxLat - r * latStep; // Top to bottom
        for (int c = 0; c < cols; c++) {
            double lon = minLon + c * lonStep;
            grid[r][c] = getDepth(lat, lon);
        }
    }

    return grid;
}

bool GEBCOReader::isLand(double lat, double lon) const {
    float depth = getDepth(lat, lon);
    if (std::isnan(depth)) return false;
    return depth > 0.0f;
}
