#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <cmath>

// Reads GEBCO global bathymetry data and provides depth queries.
// Supports two data formats:
//   1. GeoTIFF files (requires WITH_GDAL) - full resolution ~450m
//   2. Pre-processed binary depth grids (no dependencies) - produced by tools/convert_gebco.py
//
// Binary depth grid format (.gebco.bin):
//   [float64] min_lon, max_lon, min_lat, max_lat
//   [int32]   width, height
//   [int16]   data[width*height]  (row-major, top-to-bottom, metres, negative=underwater)
class GEBCOReader {
public:
    // Load a pre-processed binary depth grid (no GDAL needed)
    bool loadBinaryGrid(const std::string& binPath);

    // Load all .gebco.bin files from a directory
    bool loadBinaryDirectory(const std::string& dirPath);

#ifdef WITH_GDAL
    // Load a GEBCO GeoTIFF tile (requires GDAL)
    bool loadGeoTIFF(const std::string& tifPath);

    // Load all .tif files from a directory
    bool loadGeoTIFFDirectory(const std::string& dirPath);
#endif

    // Query depth at a point. Returns depth in metres (negative = below sea level).
    // Returns NaN if point is outside loaded data.
    float getDepth(double lat, double lon) const;

    // Query depths for a rectangular area (for heightmap generation).
    // Returns a row-major 2D grid [row][col].
    std::vector<std::vector<float>> getDepthGrid(
        double minLat, double maxLat, double minLon, double maxLon,
        int rows, int cols) const;

    // Is a point on land (elevation > 0)?
    bool isLand(double lat, double lon) const;

    // Is any data loaded?
    bool isLoaded() const { return !tiles.empty(); }

private:
    struct DepthTile {
        std::vector<int16_t> data;
        int width = 0, height = 0;
        double minLon = 0, maxLon = 0;
        double minLat = 0, maxLat = 0;
        double pixelSizeLon = 0, pixelSizeLat = 0; // Degrees per pixel
    };
    std::vector<DepthTile> tiles;

    const DepthTile* findTile(double lat, double lon) const;
    float sampleTile(const DepthTile& tile, double lat, double lon) const;
};
