#pragma once

#include <string>
#include <vector>
#include <cstdint>

// Loads and holds coastline polygon data from the compact binary format
// produced by tools/convert_coastlines.py.
//
// Binary format:
//   [uint32] polygon_count
//   For each polygon:
//     [float32] min_lon, max_lon, min_lat, max_lat  (bounding box)
//     [uint32]  vertex_count
//     [float32] lon0, lat0, lon1, lat1, ...
class CoastlineData {
public:
    struct Polygon {
        float minLon, maxLon, minLat, maxLat;
        std::vector<float> vertices; // [lon0, lat0, lon1, lat1, ...]
    };

    bool load(const std::string& path);
    bool isLoaded() const { return !polygons.empty(); }
    const std::vector<Polygon>& getPolygons() const { return polygons; }

    // Pre-filter polygons to an area of interest. Call before bulk isLand() queries.
    // Only polygons overlapping this bbox will be checked by isLand().
    void prefilter(double minLon, double maxLon, double minLat, double maxLat);

    // Point-in-polygon test: returns true if (lon, lat) is inside any land polygon.
    bool isLand(double lon, double lat) const;

private:
    std::vector<Polygon> polygons;
    std::vector<size_t> filteredIndices_; // indices into polygons (set by prefilter)
    bool useFiltered_ = false;
};
