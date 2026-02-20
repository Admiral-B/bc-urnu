#include <catch2/catch_test_macros.hpp>
#include "BarrierFloodFill.hpp"
#include <vector>
#include <cmath>

// Helper: create a flat grid of given size filled with a value
static std::vector<float> makeGrid(int res, float fill) {
    return std::vector<float>(res * res, fill);
}

TEST_CASE("BarrierFloodFill: box barrier encloses water", "[barrier]") {
    // 20x20 grid, all water at -10m
    int res = 20;
    auto grid = makeGrid(res, -10.0f);

    // Box barrier from (8,8) to (12,12) in pixel coords
    // Convert pixel coords to lat/lon: lat = maxLat - py/(res-1) * latRange
    // lon = minLon + px/(res-1) * lonRange
    BarrierFloodFill::Bounds bounds{-3.2, -3.1, 51.4, 51.5};
    double lonRange = bounds.maxLon - bounds.minLon;
    double latRange = bounds.maxLat - bounds.minLat;

    auto pxToLon = [&](int px) { return bounds.minLon + (double)px / (res - 1) * lonRange; };
    auto pyToLat = [&](int py) { return bounds.maxLat - (double)py / (res - 1) * latRange; };

    // Box corners: (8,8), (8,12), (12,12), (12,8), close back to (8,8)
    std::vector<std::pair<double,double>> box = {
        {pyToLat(8),  pxToLon(8)},
        {pyToLat(8),  pxToLon(12)},
        {pyToLat(12), pxToLon(12)},
        {pyToLat(12), pxToLon(8)},
        {pyToLat(8),  pxToLon(8)},  // close
    };

    std::vector<std::vector<std::pair<double,double>>> barriers = {box};
    int reclaimed = BarrierFloodFill::apply(grid.data(), res, bounds, barriers);

    REQUIRE(reclaimed > 0);

    // Center of box (10,10) should now be land (> 0)
    REQUIRE(grid[10 * res + 10] > 0.0f);

    // Outside the box (0,0) should remain water
    REQUIRE(grid[0 * res + 0] <= 0.0f);
}

TEST_CASE("BarrierFloodFill: no barriers returns 0", "[barrier]") {
    int res = 10;
    auto grid = makeGrid(res, -5.0f);
    BarrierFloodFill::Bounds bounds{-3.2, -3.1, 51.4, 51.5};
    std::vector<std::vector<std::pair<double,double>>> empty;

    int reclaimed = BarrierFloodFill::apply(grid.data(), res, bounds, empty);
    REQUIRE(reclaimed == 0);

    // All should remain water
    for (int i = 0; i < res * res; i++) {
        REQUIRE(grid[i] == -5.0f);
    }
}

TEST_CASE("BarrierFloodFill: barrier that does not enclose reclaims zero area", "[barrier]") {
    // A barrier line that doesn't form an enclosure (open line across middle)
    int res = 20;
    auto grid = makeGrid(res, -10.0f);
    BarrierFloodFill::Bounds bounds{-3.2, -3.1, 51.4, 51.5};
    double lonRange = bounds.maxLon - bounds.minLon;
    double latRange = bounds.maxLat - bounds.minLat;

    auto pxToLon = [&](int px) { return bounds.minLon + (double)px / (res - 1) * lonRange; };
    auto pyToLat = [&](int py) { return bounds.maxLat - (double)py / (res - 1) * latRange; };

    // Horizontal line at row 10, from col 5 to col 15 (doesn't connect to edges or form a loop)
    std::vector<std::pair<double,double>> line = {
        {pyToLat(10), pxToLon(5)},
        {pyToLat(10), pxToLon(15)},
    };

    std::vector<std::vector<std::pair<double,double>>> barriers = {line};
    int reclaimed = BarrierFloodFill::apply(grid.data(), res, bounds, barriers);

    // Open line cannot enclose any water area -- reclaimedCount should be 0
    // (barrier boundary pixels themselves are set to barrierHeight but don't count)
    REQUIRE(reclaimed == 0);

    // Water on both sides of the barrier should remain water
    REQUIRE(grid[5 * res + 10] <= 0.0f);  // above barrier
    REQUIRE(grid[15 * res + 10] <= 0.0f); // below barrier
}

TEST_CASE("BarrierFloodFill: IEEE 754 -0.0f handled correctly", "[barrier]") {
    // Grid filled with -0.0f (happens when defaultSeaDepth=0.0 produces -0.0f)
    int res = 10;
    auto grid = makeGrid(res, -0.0f);

    // Verify our fill actually produced -0.0f
    REQUIRE(std::signbit(grid[0]));

    // Box barrier enclosing center
    BarrierFloodFill::Bounds bounds{0.0, 1.0, 0.0, 1.0};

    auto pxToLon = [&](int px) { return (double)px / (res - 1); };
    auto pyToLat = [&](int py) { return 1.0 - (double)py / (res - 1); };

    std::vector<std::pair<double,double>> box = {
        {pyToLat(3), pxToLon(3)},
        {pyToLat(3), pxToLon(7)},
        {pyToLat(7), pxToLon(7)},
        {pyToLat(7), pxToLon(3)},
        {pyToLat(3), pxToLon(3)},
    };

    std::vector<std::vector<std::pair<double,double>>> barriers = {box};
    int reclaimed = BarrierFloodFill::apply(grid.data(), res, bounds, barriers);

    REQUIRE(reclaimed > 0);
    // Center should be reclaimed to land
    REQUIRE(grid[5 * res + 5] > 0.0f);
    // Edge should remain water (as -0.0f or 0.0f)
    REQUIRE(grid[0] <= 0.0f);
}

TEST_CASE("BarrierFloodFill: land pixels inside barrier remain land", "[barrier]") {
    int res = 20;
    auto grid = makeGrid(res, -10.0f);
    BarrierFloodFill::Bounds bounds{-3.2, -3.1, 51.4, 51.5};
    double lonRange = bounds.maxLon - bounds.minLon;
    double latRange = bounds.maxLat - bounds.minLat;

    auto pxToLon = [&](int px) { return bounds.minLon + (double)px / (res - 1) * lonRange; };
    auto pyToLat = [&](int py) { return bounds.maxLat - (double)py / (res - 1) * latRange; };

    // Put some land inside the barrier area
    grid[10 * res + 10] = 5.0f;
    grid[10 * res + 11] = 3.0f;

    // Box barrier
    std::vector<std::pair<double,double>> box = {
        {pyToLat(8),  pxToLon(8)},
        {pyToLat(8),  pxToLon(12)},
        {pyToLat(12), pxToLon(12)},
        {pyToLat(12), pxToLon(8)},
        {pyToLat(8),  pxToLon(8)},
    };

    std::vector<std::vector<std::pair<double,double>>> barriers = {box};
    BarrierFloodFill::apply(grid.data(), res, bounds, barriers);

    // Original land values should be preserved (not overwritten)
    REQUIRE(grid[10 * res + 10] == 5.0f);
    REQUIRE(grid[10 * res + 11] == 3.0f);
}

TEST_CASE("BarrierFloodFill: null grid returns 0", "[barrier]") {
    BarrierFloodFill::Bounds bounds{0.0, 1.0, 0.0, 1.0};
    std::vector<std::vector<std::pair<double,double>>> barriers = {{{0.5, 0.5}, {0.5, 0.6}}};
    REQUIRE(BarrierFloodFill::apply(nullptr, 10, bounds, barriers) == 0);
}

TEST_CASE("BarrierFloodFill: custom reclaim and barrier heights", "[barrier]") {
    int res = 20;
    auto grid = makeGrid(res, -10.0f);
    BarrierFloodFill::Bounds bounds{0.0, 1.0, 0.0, 1.0};

    auto pxToLon = [&](int px) { return (double)px / (res - 1); };
    auto pyToLat = [&](int py) { return 1.0 - (double)py / (res - 1); };

    std::vector<std::pair<double,double>> box = {
        {pyToLat(6),  pxToLon(6)},
        {pyToLat(6),  pxToLon(14)},
        {pyToLat(14), pxToLon(14)},
        {pyToLat(14), pxToLon(6)},
        {pyToLat(6),  pxToLon(6)},
    };

    float reclaimH = 0.5f;
    float barrierH = 3.5f;

    std::vector<std::vector<std::pair<double,double>>> barriers = {box};
    BarrierFloodFill::apply(grid.data(), res, bounds, barriers, reclaimH, barrierH);

    // Center (reclaimed) should be at reclaimHeight
    REQUIRE(grid[10 * res + 10] == reclaimH);

    // A barrier boundary pixel should be at barrierHeight
    // The box passes through row 6, col 6-14. Check a pixel on the boundary.
    bool foundBarrierPixel = false;
    for (int px = 6; px <= 14; px++) {
        if (grid[6 * res + px] == barrierH) {
            foundBarrierPixel = true;
            break;
        }
    }
    REQUIRE(foundBarrierPixel);
}

TEST_CASE("BarrierFloodFill: headland + barrier encloses bay", "[barrier]") {
    // Simulates two headlands with land and a barrier connecting across the bay mouth
    int res = 20;
    auto grid = makeGrid(res, -10.0f);
    BarrierFloodFill::Bounds bounds{-3.2, -3.1, 51.4, 51.5};
    double lonRange = bounds.maxLon - bounds.minLon;
    double latRange = bounds.maxLat - bounds.minLat;

    auto pxToLon = [&](int px) { return bounds.minLon + (double)px / (res - 1) * lonRange; };
    auto pyToLat = [&](int py) { return bounds.maxLat - (double)py / (res - 1) * latRange; };

    // Create two headlands (land masses) that almost close a bay
    // Left headland: column 0-5, rows 5-15
    for (int py = 5; py <= 15; py++)
        for (int px = 0; px <= 5; px++)
            grid[py * res + px] = 2.0f;

    // Right headland: column 14-19, rows 5-15
    for (int py = 5; py <= 15; py++)
        for (int px = 14; px <= 19; px++)
            grid[py * res + px] = 2.0f;

    // Top land strip connecting headlands (seals the bay on three sides)
    for (int px = 0; px <= 19; px++)
        grid[5 * res + px] = 2.0f;

    // Barrier across the bottom mouth (row 15, col 5 to col 14)
    std::vector<std::pair<double,double>> barrier = {
        {pyToLat(15), pxToLon(5)},
        {pyToLat(15), pxToLon(14)},
    };

    std::vector<std::vector<std::pair<double,double>>> barriers = {barrier};
    int reclaimed = BarrierFloodFill::apply(grid.data(), res, bounds, barriers);

    REQUIRE(reclaimed > 0);

    // Water inside the bay (row 10, col 10) should become land
    REQUIRE(grid[10 * res + 10] > 0.0f);

    // Water outside the bay (row 18, col 10) should remain water
    REQUIRE(grid[18 * res + 10] <= 0.0f);
}

TEST_CASE("BarrierFloodFill: endpoint snapping connects barrier to nearby land", "[barrier]") {
    // Simulates a barrage scenario like Cardiff Bay: barrier endpoints don't quite
    // reach the coastline (Natural Earth coastline imprecision), leaving gaps that
    // BFS flood would enter through. Endpoint snapping should close these gaps.
    int res = 30;
    auto grid = makeGrid(res, -10.0f);
    BarrierFloodFill::Bounds bounds{-3.2, -3.1, 51.4, 51.5};
    double lonRange = bounds.maxLon - bounds.minLon;
    double latRange = bounds.maxLat - bounds.minLat;

    auto pxToLon = [&](int px) { return bounds.minLon + (double)px / (res - 1) * lonRange; };
    auto pyToLat = [&](int py) { return bounds.maxLat - (double)py / (res - 1) * latRange; };

    // Left headland: col 0-6, rows 8-22
    for (int py = 8; py <= 22; py++)
        for (int px = 0; px <= 6; px++)
            grid[py * res + px] = 2.0f;

    // Right headland: col 23-29, rows 8-22
    for (int py = 8; py <= 22; py++)
        for (int px = 23; px <= 29; px++)
            grid[py * res + px] = 2.0f;

    // Top land connecting headlands (seals the bay on three sides)
    for (int px = 0; px <= 29; px++)
        grid[8 * res + px] = 2.0f;

    // Barrier from col 10 to col 20 at row 22 -- gaps of 3 pixels on each side
    // Left gap: col 7,8,9 (land ends at col 6, barrier starts at col 10)
    // Right gap: col 21,22 (barrier ends at col 20, land starts at col 23)
    std::vector<std::pair<double,double>> barrier = {
        {pyToLat(22), pxToLon(10)},
        {pyToLat(22), pxToLon(20)},
    };

    std::vector<std::vector<std::pair<double,double>>> barriers = {barrier};
    int reclaimed = BarrierFloodFill::apply(grid.data(), res, bounds, barriers);

    REQUIRE(reclaimed > 0);

    // Water inside the bay (row 15, col 15) should be reclaimed
    REQUIRE(grid[15 * res + 15] > 0.0f);

    // Water outside the bay (row 27, col 15) should remain water
    REQUIRE(grid[27 * res + 15] <= 0.0f);
}

TEST_CASE("BarrierFloodFill: snapping does not trigger when endpoint already on land", "[barrier]") {
    // When barrier endpoint is already on land, snapping should not alter the result
    // This is the same as the existing headland test but verifies snapping is benign
    int res = 20;
    auto grid = makeGrid(res, -10.0f);
    BarrierFloodFill::Bounds bounds{-3.2, -3.1, 51.4, 51.5};
    double lonRange = bounds.maxLon - bounds.minLon;
    double latRange = bounds.maxLat - bounds.minLat;

    auto pxToLon = [&](int px) { return bounds.minLon + (double)px / (res - 1) * lonRange; };
    auto pyToLat = [&](int py) { return bounds.maxLat - (double)py / (res - 1) * latRange; };

    // Left headland: col 0-5, rows 5-15
    for (int py = 5; py <= 15; py++)
        for (int px = 0; px <= 5; px++)
            grid[py * res + px] = 2.0f;

    // Right headland: col 14-19, rows 5-15
    for (int py = 5; py <= 15; py++)
        for (int px = 14; px <= 19; px++)
            grid[py * res + px] = 2.0f;

    // Top land
    for (int px = 0; px <= 19; px++)
        grid[5 * res + px] = 2.0f;

    // Barrier endpoints directly on land (col 5 and col 14)
    std::vector<std::pair<double,double>> barrier = {
        {pyToLat(15), pxToLon(5)},
        {pyToLat(15), pxToLon(14)},
    };

    // Save copy of grid before apply
    std::vector<float> gridBefore(grid.begin(), grid.end());

    std::vector<std::vector<std::pair<double,double>>> barriers = {barrier};
    int reclaimed = BarrierFloodFill::apply(grid.data(), res, bounds, barriers);

    REQUIRE(reclaimed > 0);
    REQUIRE(grid[10 * res + 10] > 0.0f);
    REQUIRE(grid[18 * res + 10] <= 0.0f);

    // Land pixels should not have been modified
    for (int py = 5; py <= 15; py++)
        for (int px = 0; px <= 5; px++)
            REQUIRE(grid[py * res + px] == gridBefore[py * res + px]);
}
