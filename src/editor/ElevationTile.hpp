#pragma once

#include <string>
#include <vector>
#include <functional>

// Downloads AWS Terrain Tiles (Terrarium PNG format) for a bounding box,
// decodes RGB to float elevation in metres, and resamples to a target grid.
// URL: https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png
// Encoding: elevation_m = R * 256 + G + B / 256 - 32768
// Sources: SRTM 30m (land), 3DEP (US), ETOPO1 (ocean), EU-DEM, ArcticDEM.
class ElevationTile {
public:
    // Progress callback: (tilesCompleted, totalTiles) -> return false to cancel
    using ProgressCallback = std::function<bool(int, int)>;

    // Download Terrarium PNG tiles, decode to float elevation grid.
    // Returns resolution x resolution float array (row-major, north-to-south).
    // Each float is elevation in metres (positive = land, negative = water).
    // Returns empty vector on failure or cancellation.
    static std::vector<float> generate(
        double minLat, double maxLat, double minLon, double maxLon,
        int zoom, int resolution,
        const std::string& cacheDir,
        ProgressCallback progress = nullptr);

    // Suggest zoom level for target heightmap resolution.
    // Capped at 12 (elevation doesn't need satellite-level detail).
    static int suggestZoom(double minLat, double maxLat,
                           double minLon, double maxLon,
                           int targetSize);
};
