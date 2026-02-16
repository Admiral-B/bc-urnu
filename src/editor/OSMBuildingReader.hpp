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
};

class OSMBuildingReader {
public:
    using ProgressCallback = std::function<void(const std::string&)>;

    // Query Overpass API for building footprints in bounding box (blocking).
    bool query(double minLat, double maxLat, double minLon, double maxLon,
               ProgressCallback progress = nullptr);

    const std::vector<BuildingFootprint>& getBuildings() const { return buildings; }

    bool hasData() const { return queryDone; }
    const std::string& getError() const { return errorMsg; }

    // Cache: save/load building footprints to/from a simple text file.
    // Avoids re-querying Overpass for the same area.
    bool saveCache(const std::string& path) const;
    bool loadCache(const std::string& path);

private:
    std::vector<BuildingFootprint> buildings;
    bool queryDone = false;
    std::string errorMsg;

    static std::vector<uint8_t> httpPost(const std::string& url,
                                          const std::string& body,
                                          const std::string& userAgent);
    bool parseResponse(const std::string& jsonStr);

    static float estimateHeight(const std::string& heightStr,
                                const std::string& levelsStr,
                                const std::string& type);
    static std::string classifyType(const std::string& osmType);
};
