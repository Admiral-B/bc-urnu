#pragma once

#include <string>

// Uses GDAL/OGR to load the OSM-derived land polygon shapefile
// (from osmdata.openstreetmap.de) and rasterize land areas onto a heightmap grid.
// Requires WITH_GDAL. Falls back gracefully (hasData() returns false) when
// GDAL is unavailable or shapefile not found.

class OSMLandPolygons {
public:
    OSMLandPolygons();
    ~OSMLandPolygons();

    // Load shapefile (e.g., "Data/land_polygons/land_polygons.shp").
    // Calls GDALAllRegister() internally if not yet done.
    bool load(const std::string& shapefilePath);

    // Rasterize land polygons within bounds onto a float grid.
    // Sets land pixels to landHeight, leaves other pixels unchanged.
    // Returns number of land pixels set.
    int rasterize(float* grid, int resolution,
                  double minLon, double maxLon,
                  double minLat, double maxLat,
                  float landHeight = 2.0f);

    bool hasData() const { return loaded; }

    void close();

private:
    bool loaded = false;
    void* dataset = nullptr; // GDALDatasetH, void* to avoid GDAL in header
};
