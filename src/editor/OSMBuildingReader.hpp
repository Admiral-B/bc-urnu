#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <utility>

struct BuildingFootprint {
    std::vector<std::pair<double, double>> outline; // lat/lon polygon (closed ring)
    float height = 9.0f;      // metres (default ~3 storeys)
    std::string type;          // residential/commercial/industrial/church/...
    std::string name;
    bool isStructure = false;  // true for harbour structures (pier, breakwater, dam, etc.)
};

class OSMBuildingReader {
public:
    using ProgressCallback = std::function<void(const std::string&)>;

    // Query Overpass API for building footprints in bounding box (blocking).
    bool query(double minLat, double maxLat, double minLon, double maxLon,
               ProgressCallback progress = nullptr);

    const std::vector<BuildingFootprint>& getBuildings() const { return buildings; }

    // Barrier polylines (original centerlines of dams/breakwaters, before polygon buffering).
    // Suitable for flood-fill barrier detection without a separate Overpass query.
    const std::vector<std::vector<std::pair<double,double>>>& getBarrierLines() const { return barrierLines; }

    // Island outlines (closed polygons from place=island/islet ways).
    // Used to supplement Natural Earth coastlines when OSM land polygon shapefile
    // is not available. Each entry is a closed lat/lon ring.
    const std::vector<std::vector<std::pair<double,double>>>& getIslandPolygons() const { return islandPolygons; }

    bool hasData() const { return queryDone; }
    const std::string& getError() const { return errorMsg; }

    // Cache: save/load building footprints to/from a simple text file.
    // Avoids re-querying Overpass for the same area.
    bool saveCache(const std::string& path) const;
    bool loadCache(const std::string& path);

    // HTTP POST utility (reusable for other Overpass queries)
    static std::vector<uint8_t> httpPost(const std::string& url,
                                          const std::string& body,
                                          const std::string& userAgent);

private:
    std::vector<BuildingFootprint> buildings;
    std::vector<std::vector<std::pair<double,double>>> barrierLines;
    std::vector<std::vector<std::pair<double,double>>> islandPolygons;
    bool queryDone = false;
    std::string errorMsg;
    bool parseResponse(const std::string& jsonStr);

    static float estimateHeight(const std::string& heightStr,
                                const std::string& levelsStr,
                                const std::string& type);
    static std::string classifyType(const std::string& osmType);
};
