#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <functional>

// Global Building Atlas tile downloader and spatial index.
// Downloads 5x5 degree GeoJSON tiles from HuggingFace, parses building
// polygons with ML-estimated heights, and provides fast centroid lookup.
class GBATileDownloader {
public:
    struct GBABuilding {
        std::vector<std::pair<double, double>> outline; // EPSG:3857 coordinates
        double centroidX = 0.0; // EPSG:3857
        double centroidY = 0.0; // EPSG:3857
        float height = 0.0f;    // metres
        float variance = 0.0f;  // height estimation uncertainty
    };

    using ProgressCallback = std::function<void(const std::string&)>;

    explicit GBATileDownloader(const std::string& cacheDir);

    // Load GBA data for a lat/lon bounding box (WGS84).
    // Downloads 5x5 degree tile(s) from HuggingFace if not cached.
    bool loadForBounds(double minLat, double maxLat,
                       double minLon, double maxLon,
                       ProgressCallback progress = nullptr);

    // Find nearest GBA building to a point in EPSG:3857 coordinates.
    // Returns nullptr if no building found within searchRadiusM metres.
    const GBABuilding* findNearest(double x3857, double y3857,
                                    double searchRadiusM = 50.0) const;

    // Find GBA building containing a point in EPSG:3857 coordinates.
    const GBABuilding* findContaining(double x3857, double y3857) const;

    int buildingCount() const { return (int)buildings.size(); }
    bool hasData() const { return !buildings.empty(); }

    // Compute the GBA tile name for a lat/lon position.
    // GBA uses 5x5 degree tiles with names like "w5_n55_w0_n50"
    // meaning west boundary 5W, north boundary 55N, east boundary 0, south boundary 50N.
    static std::string tileName(double lat, double lon);

    // Get all tile names covering a bounding box.
    static std::vector<std::string> tileNamesForBounds(double minLat, double maxLat,
                                                        double minLon, double maxLon);

    // Parse GeoJSON string into buildings. Public for testing.
    bool parseGeoJSON(const std::string& json);

private:
    std::string cacheDir;
    std::vector<GBABuilding> buildings;

    // Grid-based spatial index: cellSize in EPSG:3857 metres
    static constexpr double CELL_SIZE = 100.0; // 100m grid cells
    std::unordered_map<int64_t, std::vector<int>> spatialGrid;

    void buildSpatialIndex();
    int64_t gridKey(double x, double y) const;
    std::vector<int64_t> neighborKeys(double x, double y, double radius) const;

    bool downloadTile(const std::string& name, ProgressCallback progress);
    std::string tileCachePath(const std::string& name) const;
    static std::string buildHuggingFaceUrl(const std::string& tileName);
};
