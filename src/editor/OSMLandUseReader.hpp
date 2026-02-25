#pragma once

#include <string>
#include <vector>
#include <functional>
#include <utility>
#include <cstdint>

// Land use categories for terrain texturing
enum class LandUseType : uint8_t {
    Unclassified = 0,
    Forest,         // 1: landuse=forest, natural=wood
    Residential,    // 2: landuse=residential
    Industrial,     // 3: landuse=industrial
    Commercial,     // 4: landuse=commercial
    Farmland,       // 5: landuse=farmland
    Grass,          // 6: landuse=grass/meadow, leisure=park/garden
    Heath,          // 7: natural=heath/scrub
    Beach,          // 8: natural=beach
    Rock,           // 9: natural=rock/cliff/scree
    Construction,   // 10: landuse=construction
    Quarry,         // 11: landuse=quarry
    Allotments,     // 12: landuse=allotments
    Cemetery,       // 13: landuse=cemetery
    Parking,        // 14: landuse=parking, amenity=parking
    Wetland,        // 15: natural=wetland
    Mud,            // 16: natural=mud
    Shingle,        // 17: natural=shingle
    TidalFlat,      // 18: natural=tidal_flat
    Waterfront,     // 19: man-made quay/dock edge (set by EditorApp from dock water polygons)
    COUNT
};

struct LandUsePolygon {
    std::vector<std::pair<double, double>> outline; // lat/lon closed ring
    LandUseType type = LandUseType::Unclassified;
};

class OSMLandUseReader {
public:
    using ProgressCallback = std::function<void(const std::string&)>;

    // Query Overpass API for land use polygons in bounding box (blocking).
    bool query(double minLat, double maxLat, double minLon, double maxLon,
               ProgressCallback progress = nullptr);

    const std::vector<LandUsePolygon>& getPolygons() const { return polygons; }

    bool hasData() const { return queryDone; }
    const std::string& getError() const { return errorMsg; }

    // Rasterize all polygons into a grid (row-major, row 0 = north).
    // Grid must be pre-allocated to resolution*resolution bytes.
    // Last-write-wins for overlapping polygons.
    void rasterize(uint8_t* grid, int resolution,
                   double minLon, double maxLon,
                   double minLat, double maxLat) const;

    // Parse Overpass JSON response (public for testing)
    bool parseResponse(const std::string& jsonStr);

private:
    std::vector<LandUsePolygon> polygons;
    bool queryDone = false;
    std::string errorMsg;

    static LandUseType classify(const std::string& landuse,
                                const std::string& natural,
                                const std::string& leisure);
};
