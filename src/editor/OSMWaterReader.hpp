#pragma once

#include <string>
#include <vector>
#include <functional>
#include <utility>

struct WaterPolygon {
    std::vector<std::pair<double, double>> outline; // lat/lon closed ring
    std::string type; // sea, lake, river, dock, reservoir, basin
};

class OSMWaterReader {
public:
    using ProgressCallback = std::function<void(const std::string&)>;

    // Query Overpass API for water polygons AND barrier ways in bounding box (blocking).
    // Barriers are dams (waterway=dam) and breakwaters (man_made=breakwater).
    bool query(double minLat, double maxLat, double minLon, double maxLon,
               ProgressCallback progress = nullptr);

    const std::vector<WaterPolygon>& getWaterAreas() const { return waterAreas; }

    // Barrier geometries (dams, breakwaters) as lat/lon polylines
    const std::vector<std::vector<std::pair<double,double>>>& getBarriers() const { return barriers; }

    bool hasData() const { return queryDone; }
    const std::string& getError() const { return errorMsg; }

    // Point-in-polygon test: is (lat, lon) inside any water polygon?
    bool isWater(double lat, double lon) const;

    // Cache: save/load to avoid re-querying
    bool saveCache(const std::string& path) const;
    bool loadCache(const std::string& path);

    // Parse Overpass JSON response (public for testing)
    bool parseResponse(const std::string& jsonStr);

private:
    std::vector<WaterPolygon> waterAreas;
    std::vector<std::vector<std::pair<double,double>>> barriers; // lat/lon polylines
    bool queryDone = false;
    std::string errorMsg;

    static bool pointInRing(double lat, double lon,
                            const std::vector<std::pair<double, double>>& ring);
    static std::string classifyWaterType(const std::string& natural,
                                         const std::string& waterTag,
                                         const std::string& waterway);
};
