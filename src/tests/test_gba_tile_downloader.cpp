#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "editor/GBATileDownloader.hpp"
#include "editor/CoordinateProjection.hpp"

using Catch::Matchers::WithinAbs;

// --- Burst 21: Tile name from lat/lon ---

TEST_CASE("GBA tile name for UK (London)", "[GBA]") {
    // London: 51.5N, -0.1W -> tile [-5,0] x [50,55]
    auto name = GBATileDownloader::tileName(51.5, -0.1);
    REQUIRE(name == "w5_n55_w0_n50");
}

TEST_CASE("GBA tile name for positive lon (Berlin)", "[GBA]") {
    // Berlin: 52.5N, 13.4E -> tile [10,15] x [50,55]
    auto name = GBATileDownloader::tileName(52.5, 13.4);
    REQUIRE(name == "e10_n55_e15_n50");
}

TEST_CASE("GBA tile name for southern hemisphere", "[GBA]") {
    // Sydney: -33.9S, 151.2E -> tile [150,155] x [-35,-30]
    auto name = GBATileDownloader::tileName(-33.9, 151.2);
    REQUIRE(name == "e150_s30_e155_s35");
}

TEST_CASE("GBA tile names for bounding box spanning two tiles", "[GBA]") {
    // Box from 49N,-6W to 56N,2E -> should cover at least 2 tiles
    auto names = GBATileDownloader::tileNamesForBounds(49.0, 56.0, -6.0, 2.0);
    REQUIRE(names.size() >= 2);
    // Should include the UK tiles
    bool hasW5 = false, hasW0 = false;
    for (auto& n : names) {
        if (n.find("w5_") != std::string::npos || n.find("w10_") != std::string::npos) hasW5 = true;
        if (n.find("w0_") != std::string::npos || n.find("e0_") != std::string::npos) hasW0 = true;
    }
    REQUIRE((hasW5 || hasW0)); // At least one of these tiles
}

// --- Burst 22: GeoJSON parsing ---

TEST_CASE("GBA GeoJSON parser extracts buildings with height and variance", "[GBA]") {
    std::string geojson = R"({
        "type": "FeatureCollection",
        "features": [
            {
                "type": "Feature",
                "geometry": {
                    "type": "Polygon",
                    "coordinates": [[[100.0, 200.0], [110.0, 200.0], [110.0, 210.0], [100.0, 210.0], [100.0, 200.0]]]
                },
                "properties": {
                    "height": 12.5,
                    "var": 2.3
                }
            },
            {
                "type": "Feature",
                "geometry": {
                    "type": "Polygon",
                    "coordinates": [[[200.0, 300.0], [210.0, 300.0], [210.0, 310.0], [200.0, 300.0]]]
                },
                "properties": {
                    "height": 8.0,
                    "var": 1.1
                }
            }
        ]
    })";

    GBATileDownloader gba("/tmp/test_gba");
    REQUIRE(gba.parseGeoJSON(geojson));
    REQUIRE(gba.buildingCount() == 2);
}

// --- Burst 23: Spatial index finds nearest polygon ---

TEST_CASE("GBA spatial index finds nearest building by centroid", "[GBA]") {
    std::string geojson = R"({
        "type": "FeatureCollection",
        "features": [
            {
                "type": "Feature",
                "geometry": {
                    "type": "Polygon",
                    "coordinates": [[[100.0, 200.0], [110.0, 200.0], [110.0, 210.0], [100.0, 210.0], [100.0, 200.0]]]
                },
                "properties": { "height": 15.0, "var": 1.0 }
            },
            {
                "type": "Feature",
                "geometry": {
                    "type": "Polygon",
                    "coordinates": [[[500.0, 600.0], [510.0, 600.0], [510.0, 610.0], [500.0, 610.0], [500.0, 600.0]]]
                },
                "properties": { "height": 25.0, "var": 0.5 }
            }
        ]
    })";

    GBATileDownloader gba("/tmp/test_gba");
    REQUIRE(gba.parseGeoJSON(geojson));

    // Query near first building centroid (105, 205)
    auto* nearest = gba.findNearest(106.0, 206.0, 50.0);
    REQUIRE(nearest != nullptr);
    REQUIRE_THAT(nearest->height, WithinAbs(15.0, 0.1));

    // Query near second building centroid (505, 605)
    nearest = gba.findNearest(504.0, 604.0, 50.0);
    REQUIRE(nearest != nullptr);
    REQUIRE_THAT(nearest->height, WithinAbs(25.0, 0.1));

    // Query far from both buildings
    nearest = gba.findNearest(9999.0, 9999.0, 50.0);
    REQUIRE(nearest == nullptr);
}

// --- Burst 25: EPSG coordinate projection ---

TEST_CASE("EPSG:4326 to EPSG:3857 reprojection accuracy", "[Projection]") {
    // Known values: London (51.5074, -0.1278)
    // Expected EPSG:3857: approximately x=-14226, y=6711544
    double x = CoordinateProjection::lonToX(-0.1278);
    double y = CoordinateProjection::latToY(51.5074);

    REQUIRE_THAT(x, WithinAbs(-14226.0, 50.0));
    REQUIRE_THAT(y, WithinAbs(6711544.0, 500.0));

    // Round-trip
    double lon = CoordinateProjection::xToLon(x);
    double lat = CoordinateProjection::yToLat(y);
    REQUIRE_THAT(lon, WithinAbs(-0.1278, 0.0001));
    REQUIRE_THAT(lat, WithinAbs(51.5074, 0.0001));
}

TEST_CASE("EPSG projection at equator/prime meridian", "[Projection]") {
    double x = CoordinateProjection::lonToX(0.0);
    double y = CoordinateProjection::latToY(0.0);
    REQUIRE_THAT(x, WithinAbs(0.0, 0.01));
    REQUIRE_THAT(y, WithinAbs(0.0, 0.01));
}
