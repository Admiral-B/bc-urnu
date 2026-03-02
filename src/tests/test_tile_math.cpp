#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "../editor/TileMath.hpp"

using namespace TileMath;

TEST_CASE("Tile coordinates at zoom 0", "[tiles]") {
    // At zoom 0, entire world is one tile
    REQUIRE(lonToTileX(-180.0, 0) == 0);
    REQUIRE(lonToTileX(0.0, 0) == 0);
    REQUIRE(latToTileY(0.0, 0) == 0);
}

TEST_CASE("Tile coordinates at zoom 1", "[tiles]") {
    // At zoom 1, 2x2 = 4 tiles
    REQUIRE(lonToTileX(-180.0, 1) == 0);
    REQUIRE(lonToTileX(-0.01, 1) == 0);  // Just west of prime meridian
    REQUIRE(lonToTileX(0.01, 1) == 1);   // Just east of prime meridian
    REQUIRE(lonToTileX(179.99, 1) == 1);

    REQUIRE(latToTileY(85.0, 1) == 0);   // Near north pole
    REQUIRE(latToTileY(-85.0, 1) == 1);  // Near south pole
}

TEST_CASE("Tile coordinates at zoom 2", "[tiles]") {
    // At zoom 2, 4x4 = 16 tiles
    REQUIRE(lonToTileX(0.0, 2) == 2);     // Prime meridian
    REQUIRE(latToTileY(0.0, 2) == 2);     // Equator
    REQUIRE(lonToTileX(-122.4, 2) == 0);  // San Francisco
}

TEST_CASE("Known locations at zoom 10", "[tiles]") {
    int z = 10;

    // London: 51.5, -0.1
    int lx = lonToTileX(-0.1, z);
    int ly = latToTileY(51.5, z);
    REQUIRE(lx >= 510);
    REQUIRE(lx <= 512);
    REQUIRE(ly >= 339);
    REQUIRE(ly <= 341);

    // Swinomish Channel: 48.45, -122.5
    int sx = lonToTileX(-122.5, z);
    int sy = latToTileY(48.45, z);
    REQUIRE(sx >= 160);
    REQUIRE(sx <= 165);
    REQUIRE(sy >= 350);
    REQUIRE(sy <= 360);
}

TEST_CASE("Tile X round-trip", "[tiles]") {
    // tileXToLon(lonToTileX(lon, z), z) should be close to lon
    // (within one tile width)
    for (int z = 1; z <= 15; z++) {
        double lon = -122.4;
        int tx = lonToTileX(lon, z);
        double lonBack = tileXToLon(tx, z);
        double tileWidth = 360.0 / (1 << z);
        REQUIRE(std::abs(lonBack - lon) <= tileWidth);
    }
}

TEST_CASE("Tile Y round-trip", "[tiles]") {
    for (int z = 1; z <= 15; z++) {
        double lat = 51.5;
        int ty = latToTileY(lat, z);
        double latBack = tileYToLat(ty, z);
        double nextLat = tileYToLat(ty + 1, z);
        double tileHeight = std::abs(latBack - nextLat);
        REQUIRE(std::abs(latBack - lat) <= tileHeight);
    }
}

TEST_CASE("pixelToLatLon and latLonToPixel round-trip", "[tiles]") {
    double centerLat = 51.5;
    double centerLon = -0.1;
    int zoom = 12;

    // Test a point offset from center
    double testLat = 51.52;
    double testLon = -0.08;

    PixelPos px = latLonToPixel(centerLat, centerLon, zoom, testLat, testLon);
    LatLon ll = pixelToLatLon(centerLat, centerLon, zoom, px.x, px.y);

    // Should be within 1 pixel (~1/256 of a tile)
    double tileWidthDeg = 360.0 / (1 << zoom);
    double pixelWidthDeg = tileWidthDeg / 256.0;
    REQUIRE(std::abs(ll.lon - testLon) < pixelWidthDeg * 2);
    REQUIRE(std::abs(ll.lat - testLat) < pixelWidthDeg * 2);
}

TEST_CASE("pixelToLatLon center returns center", "[tiles]") {
    double centerLat = 48.45;
    double centerLon = -122.5;
    int zoom = 14;

    LatLon ll = pixelToLatLon(centerLat, centerLon, zoom, 0, 0);
    REQUIRE_THAT(ll.lat, Catch::Matchers::WithinAbs(centerLat, 0.0001));
    REQUIRE_THAT(ll.lon, Catch::Matchers::WithinAbs(centerLon, 0.0001));
}

TEST_CASE("latLonToPixel center returns zero offset", "[tiles]") {
    double centerLat = 48.45;
    double centerLon = -122.5;
    int zoom = 14;

    PixelPos px = latLonToPixel(centerLat, centerLon, zoom, centerLat, centerLon);
    REQUIRE(px.x == 0);
    REQUIRE(px.y == 0);
}

TEST_CASE("clampLat clamps extreme values", "[tiles]") {
    REQUIRE(clampLat(90.0) == 85.0511);
    REQUIRE(clampLat(-90.0) == -85.0511);
    REQUIRE(clampLat(50.0) == 50.0);
    REQUIRE(clampLat(0.0) == 0.0);
}

TEST_CASE("Tile coordinates at max zoom", "[tiles]") {
    int z = 19;
    int maxTile = (1 << z) - 1;

    REQUIRE(lonToTileX(-180.0, z) == 0);
    REQUIRE(lonToTileX(179.99999, z) == maxTile);
    // Values beyond Mercator limit (85.0511) get clamped to tile 0 / maxTile
    REQUIRE(latToTileY(90.0, z) == 0);
    REQUIRE(latToTileY(-90.0, z) == maxTile);
}

TEST_CASE("tileXToLon boundary values", "[tiles]") {
    REQUIRE_THAT(tileXToLon(0, 0), Catch::Matchers::WithinAbs(-180.0, 0.001));
    REQUIRE_THAT(tileXToLon(0, 1), Catch::Matchers::WithinAbs(-180.0, 0.001));
    REQUIRE_THAT(tileXToLon(1, 1), Catch::Matchers::WithinAbs(0.0, 0.001));
}

TEST_CASE("tileYToLat boundary values", "[tiles]") {
    // At zoom 0, tile 0 top edge should be ~85.05 degrees
    double topLat = tileYToLat(0, 0);
    REQUIRE(topLat > 85.0);
    REQUIRE(topLat < 86.0);
}
