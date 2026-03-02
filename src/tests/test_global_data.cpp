#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <filesystem>

// Include the editor sources directly (they have no Irrlicht runtime dependency)
#include "../editor/CoastlineData.hpp"
#include "../editor/CoastlineData.cpp"

namespace fs = std::filesystem;

// Find the bin/Data directory relative to the test executable or the repo root
static std::string findDataDir() {
    // Try common locations
    std::vector<std::string> candidates = {
        "bin/Data",
        "../bin/Data",
        "../../bin/Data",
        "../../../bin/Data",
    };
    for (const auto& dir : candidates) {
        if (fs::exists(dir + "/Coastlines/coastlines_50m.bin")) {
            return dir;
        }
    }
    return "";
}

TEST_CASE("CoastlineData loads 50m binary") {
    std::string dataDir = findDataDir();
    if (dataDir.empty()) {
        SKIP("Coastline data files not found (run tools/download_natural_earth.sh first)");
    }

    CoastlineData data;
    REQUIRE(data.load(dataDir + "/Coastlines/coastlines_50m.bin"));
    REQUIRE(data.isLoaded());

    // Should have many polygons (continents, islands)
    const auto& polygons = data.getPolygons();
    REQUIRE(polygons.size() > 100);
    REQUIRE(polygons.size() < 50000);

    // Each polygon should have vertices
    for (const auto& poly : polygons) {
        REQUIRE(poly.vertices.size() >= 6); // At least 3 vertices (lon, lat pairs)
        // Bounding box should be valid
        REQUIRE(poly.minLon <= poly.maxLon);
        REQUIRE(poly.minLat <= poly.maxLat);
        // Should be in valid geographic range
        REQUIRE(poly.minLon >= -180.5f);
        REQUIRE(poly.maxLon <= 180.5f);
        REQUIRE(poly.minLat >= -90.5f);
        REQUIRE(poly.maxLat <= 90.5f);
    }
}

TEST_CASE("CoastlineData loads 10m binary") {
    std::string dataDir = findDataDir();
    if (dataDir.empty()) {
        SKIP("Coastline data files not found");
    }

    CoastlineData data;
    REQUIRE(data.load(dataDir + "/Coastlines/coastlines_10m.bin"));
    REQUIRE(data.isLoaded());

    // 10m should have more polygons than 50m
    REQUIRE(data.getPolygons().size() > 1000);
}

TEST_CASE("CoastlineData handles missing file") {
    CoastlineData data;
    REQUIRE_FALSE(data.load("nonexistent/path/does_not_exist.bin"));
    REQUIRE_FALSE(data.isLoaded());
}

TEST_CASE("CoastlineData handles empty/corrupt file") {
    // Create a temp file with garbage data
    std::string tempPath = "test_corrupt_coastline.bin";
    {
        std::ofstream f(tempPath, std::ios::binary);
        uint32_t badCount = 999999999;
        f.write(reinterpret_cast<const char*>(&badCount), 4);
    }

    CoastlineData data;
    REQUIRE_FALSE(data.load(tempPath));

    fs::remove(tempPath);
}
