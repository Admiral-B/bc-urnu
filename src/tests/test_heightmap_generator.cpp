#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#ifdef WITH_GDAL
#include "HeightmapGenerator.hpp"

using Catch::Approx;

// ── encodeRGB ───────────────────────────────────────────────────────────────

TEST_CASE("encodeRGB sea level encodes to 32768", "[heightmap]") {
    // Height = 0 → encoded = 32768 → R=128, G=0, B=0
    std::vector<std::vector<float>> grid = {{0.0f}};
    auto rgb = HeightmapGenerator::encodeRGB(grid);
    REQUIRE(rgb.size() == 3);
    REQUIRE(rgb[0] == 128); // R = 32768 / 256
    REQUIRE(rgb[1] == 0);   // G = 32768 % 256
    REQUIRE(rgb[2] == 0);   // B = 0 (no fractional)
}

TEST_CASE("encodeRGB positive height", "[heightmap]") {
    // Height = 10.0 → encoded = 32778 → R=128, G=10, B=0
    std::vector<std::vector<float>> grid = {{10.0f}};
    auto rgb = HeightmapGenerator::encodeRGB(grid);
    REQUIRE(rgb[0] == 128); // R = 32778 / 256
    REQUIRE(rgb[1] == 10);  // G = 32778 % 256
    REQUIRE(rgb[2] == 0);
}

TEST_CASE("encodeRGB negative depth", "[heightmap]") {
    // Height = -20.0 → encoded = 32748
    // R = 32748 / 256 = 127, G = 32748 % 256 = 236 (127*256=32512, 32748-32512=236)
    std::vector<std::vector<float>> grid = {{-20.0f}};
    auto rgb = HeightmapGenerator::encodeRGB(grid);
    REQUIRE(rgb[0] == 127);
    REQUIRE(rgb[1] == 236);
    REQUIRE(rgb[2] == 0);
}

TEST_CASE("encodeRGB round-trip accuracy", "[heightmap]") {
    // Verify that encoding then decoding gets back close to original
    std::vector<float> testHeights = {-100.0f, -50.5f, -10.0f, 0.0f, 5.0f, 25.0f, 100.0f};

    for (float h : testHeights) {
        std::vector<std::vector<float>> grid = {{h}};
        auto rgb = HeightmapGenerator::encodeRGB(grid);

        // Decode: Height = R*256 + G + B/256.0 - 32768
        float decoded = static_cast<float>(rgb[0]) * 256.0f +
                        static_cast<float>(rgb[1]) +
                        static_cast<float>(rgb[2]) / 256.0f - 32768.0f;

        // Should be within 1 metre of original (B channel gives ~0.004m precision)
        REQUIRE(decoded == Approx(h).margin(1.0));
    }
}

TEST_CASE("encodeRGB multi-pixel grid", "[heightmap]") {
    std::vector<std::vector<float>> grid = {
        {0.0f, 10.0f},
        {-5.0f, 50.0f}
    };
    auto rgb = HeightmapGenerator::encodeRGB(grid);
    REQUIRE(rgb.size() == 12); // 2x2 * 3 channels
}

TEST_CASE("encodeRGB empty grid returns empty", "[heightmap]") {
    std::vector<std::vector<float>> grid;
    auto rgb = HeightmapGenerator::encodeRGB(grid);
    REQUIRE(rgb.empty());
}

// ── computeBoundsFromData ───────────────────────────────────────────────────

TEST_CASE("computeBoundsFromData with soundings", "[heightmap]") {
    HeightmapGenerator gen;
    std::vector<Sounding> soundings = {
        {-122.60, 48.30, 10.0},
        {-122.50, 48.40, 20.0},
    };
    gen.setSoundings(soundings);

    auto bounds = gen.computeBoundsFromData();
    REQUIRE(bounds.minLon < -122.60);
    REQUIRE(bounds.maxLon > -122.50);
    REQUIRE(bounds.minLat < 48.30);
    REQUIRE(bounds.maxLat > 48.40);
}

TEST_CASE("computeBoundsFromData with depth areas", "[heightmap]") {
    HeightmapGenerator gen;
    std::vector<DepthArea> areas;
    DepthArea a;
    a.minDepth = 5.0;
    a.maxDepth = 10.0;
    a.boundary = {{-122.55, 48.35}, {-122.50, 48.35}, {-122.50, 48.40}, {-122.55, 48.40}, {-122.55, 48.35}};
    areas.push_back(a);
    gen.setDepthAreas(areas);

    auto bounds = gen.computeBoundsFromData();
    REQUIRE(bounds.minLon < -122.55);
    REQUIRE(bounds.maxLon > -122.50);
}

// ── generate basic tests ────────────────────────────────────────────────────

TEST_CASE("generate with simple depth area", "[heightmap]") {
    HeightmapGenerator gen;

    // Create a depth area covering the entire bounds
    DepthArea area;
    area.minDepth = 8.0;
    area.maxDepth = 12.0;
    area.boundary = {
        {-122.60, 48.30}, {-122.40, 48.30},
        {-122.40, 48.50}, {-122.60, 48.50},
        {-122.60, 48.30} // closed
    };
    gen.setDepthAreas({area});

    HeightmapBounds bounds{-122.60, -122.40, 48.30, 48.50};
    gen.setBounds(bounds);

    HeightmapParams params;
    params.resolution = 5; // Small for testing
    auto grid = gen.generate(params);

    REQUIRE(grid.size() == 5);
    REQUIRE(grid[0].size() == 5);

    // Interior pixels should have depth ≈ -10.0 (avg of 8 and 12)
    // Check center pixel
    REQUIRE(grid[2][2] == Approx(-10.0f).margin(1.0));
}

TEST_CASE("generate land areas have positive height", "[heightmap]") {
    HeightmapGenerator gen;

    // Create a land polygon covering the area
    CoastlineSegment land;
    land.points = {
        {-122.60, 48.30}, {-122.40, 48.30},
        {-122.40, 48.50}, {-122.60, 48.50},
        {-122.60, 48.30} // closed
    };
    gen.setCoastlines({land});

    HeightmapBounds bounds{-122.60, -122.40, 48.30, 48.50};
    gen.setBounds(bounds);

    HeightmapParams params;
    params.resolution = 5;
    params.defaultLandHeight = 5.0;
    auto grid = gen.generate(params);

    // Center pixel should be land
    REQUIRE(grid[2][2] == Approx(5.0f));
}

TEST_CASE("generate empty data returns zero grid", "[heightmap]") {
    HeightmapGenerator gen;

    HeightmapBounds bounds{-122.60, -122.40, 48.30, 48.50};
    gen.setBounds(bounds);

    HeightmapParams params;
    params.resolution = 3;
    params.defaultSeaDepth = 0.0;
    auto grid = gen.generate(params);

    REQUIRE(grid.size() == 3);
    for (const auto& row : grid) {
        for (float v : row) {
            REQUIRE(v == Approx(0.0f));
        }
    }
}

// ── DEMTile / sampleDEM tests ───────────────────────────────────────────────

TEST_CASE("sampleDEM returns NaN when no tiles loaded", "[heightmap]") {
    HeightmapGenerator gen;
    float val = gen.sampleDEM(-122.5, 48.4);
    REQUIRE(std::isnan(val));
}

TEST_CASE("generate with land uses DEM when available", "[heightmap]") {
    HeightmapGenerator gen;

    // Create a land polygon covering the area
    CoastlineSegment land;
    land.points = {
        {-122.60, 48.30}, {-122.40, 48.30},
        {-122.40, 48.50}, {-122.60, 48.50},
        {-122.60, 48.30} // closed
    };
    gen.setCoastlines({land});

    // Without DEM, land should get defaultLandHeight
    HeightmapBounds bounds{-122.60, -122.40, 48.30, 48.50};
    gen.setBounds(bounds);

    HeightmapParams params;
    params.resolution = 5;
    params.defaultLandHeight = 5.0;
    auto grid = gen.generate(params);
    REQUIRE(grid[2][2] == Approx(5.0f));
}

// ── Barrier flood-fill tests ────────────────────────────────────────────────

TEST_CASE("barrier box encloses water as land", "[heightmap][barrier]") {
    // A closed barrier rectangle in an all-water grid.
    // Water inside the box is unreachable from grid edges -> becomes land.
    // Water outside the box is reachable -> stays water.
    //
    // 21x21 grid, barrier box at rows 8-12, cols 8-12.

    HeightmapGenerator gen;

    // Depth area extends beyond grid bounds to avoid ray-cast edge ambiguity
    DepthArea water;
    water.minDepth = 8.0;
    water.maxDepth = 12.0;
    water.boundary = {
        {-3.21, 51.43}, {-3.15, 51.43},
        {-3.15, 51.49}, {-3.21, 51.49},
        {-3.21, 51.43}
    };
    gen.setDepthAreas({water});

    HeightmapBounds bounds{-3.20, -3.16, 51.44, 51.48};
    gen.setBounds(bounds);

    // Barrier forms a closed rectangle.
    // On a 21x21 grid (step=0.002), rows 8-12, cols 8-12:
    //   lat 51.464 -> row 8,  lat 51.456 -> row 12
    //   lon -3.184 -> col 8,  lon -3.176 -> col 12
    std::vector<std::vector<std::pair<double,double>>> barriers = {
        {
            {51.464, -3.184}, {51.464, -3.176},
            {51.456, -3.176}, {51.456, -3.184},
            {51.464, -3.184}
        }
    };
    gen.setBarriers(barriers);

    HeightmapParams params;
    params.resolution = 21;
    params.defaultSeaDepth = 10.0;
    auto grid = gen.generate(params);

    REQUIRE(grid.size() == 21);
    REQUIRE(grid[0].size() == 21);

    // Center (row 10, col 10) is inside the barrier box -> enclosed -> land
    REQUIRE(grid[10][10] >= 0.0f);

    // Corner (row 0, col 0) is far outside the box -> open sea -> water
    REQUIRE(grid[0][0] < 0.0f);

    // Top edge center (row 0, col 10) is outside the box -> water
    REQUIRE(grid[0][10] < 0.0f);

    // Bottom edge center (row 20, col 10) is outside the box -> water
    REQUIRE(grid[20][10] < 0.0f);

    // 3 rows above barrier box -> open sea -> water
    REQUIRE(grid[5][10] < 0.0f);
}

TEST_CASE("no barriers means no flood-fill changes", "[heightmap][barrier]") {
    // Without barriers, water should stay as water everywhere
    HeightmapGenerator gen;

    DepthArea water;
    water.minDepth = 5.0;
    water.maxDepth = 15.0;
    water.boundary = {
        {-3.21, 51.43}, {-3.15, 51.43},
        {-3.15, 51.49}, {-3.21, 51.49},
        {-3.21, 51.43}
    };
    gen.setDepthAreas({water});

    HeightmapBounds bounds{-3.20, -3.16, 51.44, 51.48};
    gen.setBounds(bounds);

    // No barriers set
    HeightmapParams params;
    params.resolution = 5;
    auto grid = gen.generate(params);

    // All pixels should be water (negative or -0.0)
    for (const auto& row : grid) {
        for (float v : row) {
            REQUIRE(v <= 0.0f);
        }
    }
}

TEST_CASE("closed polygon barrier encloses water (Cardiff Bay shape)", "[heightmap][barrier]") {
    // Simulates a real-world scenario: a closed polygon barrier (like Cardiff Bay
    // Barrage, OSM way 1287700755) forming an irregular enclosure.
    // The barrier is a closed loop (first == last point) at ~40% grid extent.
    HeightmapGenerator gen;

    DepthArea water;
    water.minDepth = 8.0;
    water.maxDepth = 12.0;
    water.boundary = {
        {-3.21, 51.43}, {-3.15, 51.43},
        {-3.15, 51.49}, {-3.21, 51.49},
        {-3.21, 51.43}
    };
    gen.setDepthAreas({water});

    HeightmapBounds bounds{-3.20, -3.16, 51.44, 51.48};
    gen.setBounds(bounds);

    // Irregular closed barrier polygon (not axis-aligned), roughly pentagonal.
    // Vertices chosen so the centroid is near grid center (row~10, col~10 on 21x21).
    std::vector<std::vector<std::pair<double,double>>> barriers = {
        {
            {51.465, -3.185},   // top-left area
            {51.465, -3.175},   // top-right area
            {51.458, -3.172},   // right side
            {51.454, -3.178},   // bottom
            {51.456, -3.186},   // left side
            {51.465, -3.185}    // closed: first == last
        }
    };
    gen.setBarriers(barriers);

    HeightmapParams params;
    params.resolution = 21;
    params.defaultSeaDepth = 10.0;
    auto grid = gen.generate(params);

    // Center (inside polygon) should be land (enclosed)
    REQUIRE(grid[10][10] >= 0.0f);

    // Corners (far outside polygon) should remain water
    REQUIRE(grid[0][0] < 0.0f);
    REQUIRE(grid[20][20] < 0.0f);
    REQUIRE(grid[0][20] < 0.0f);
    REQUIRE(grid[20][0] < 0.0f);
}

TEST_CASE("barrier between two land masses encloses bay", "[heightmap][barrier]") {
    // Simulates a barrage connecting two land masses (headlands) with water
    // between them. The barrier + land together form an enclosure.
    //
    // Layout on 21x21 grid (step = 0.002 deg):
    //   Rows 0-2, cols 7-13:  connecting land strip (top)
    //   Rows 0-12, cols 0-7:  west headland
    //   Rows 0-12, cols 12-20: east headland
    //   Row 12, cols 7-12:    barrier (bottom of bay)
    //   Rows 3-11, cols 8-11: enclosed bay (water -> land)
    //   Rows 13-20:           open sea
    HeightmapGenerator gen;

    // Water covering the full area (extends beyond grid bounds)
    DepthArea water;
    water.minDepth = 5.0;
    water.maxDepth = 15.0;
    water.boundary = {
        {-3.21, 51.43}, {-3.15, 51.43},
        {-3.15, 51.49}, {-3.21, 51.49},
        {-3.21, 51.43}
    };
    gen.setDepthAreas({water});

    // West headland: cols 0-7, rows 0-12
    CoastlineSegment westLand;
    westLand.points = {
        {-3.21, 51.455}, {-3.185, 51.455},
        {-3.185, 51.49}, {-3.21, 51.49},
        {-3.21, 51.455}
    };
    // East headland: cols 12-20, rows 0-12
    CoastlineSegment eastLand;
    eastLand.points = {
        {-3.175, 51.455}, {-3.15, 51.455},
        {-3.15, 51.49}, {-3.175, 51.49},
        {-3.175, 51.455}
    };
    // Connecting strip at top: cols 7-13, rows 0-2 (bridges the gap)
    CoastlineSegment topConnect;
    topConnect.points = {
        {-3.186, 51.476}, {-3.174, 51.476},
        {-3.174, 51.49}, {-3.186, 51.49},
        {-3.186, 51.476}
    };
    gen.setCoastlines({westLand, eastLand, topConnect});

    HeightmapBounds bounds{-3.20, -3.16, 51.44, 51.48};
    gen.setBounds(bounds);

    // Barrier spans the gap between headlands (east-west) at the bay mouth
    std::vector<std::vector<std::pair<double,double>>> barriers = {
        {
            {51.456, -3.185},  // connects to west headland
            {51.456, -3.175}   // connects to east headland
        }
    };
    gen.setBarriers(barriers);

    HeightmapParams params;
    params.resolution = 21;
    params.defaultSeaDepth = 10.0;
    params.defaultLandHeight = 5.0;
    auto grid = gen.generate(params);

    // The bay area (north of barrier, between headlands, below top strip)
    // Row 6, col 10 is inside the enclosed bay
    REQUIRE(grid[6][10] >= 0.0f);

    // South of the barrier (open sea) should still be water
    // Row 18, col 10 is south of barrier, outside headlands
    REQUIRE(grid[18][10] < 0.0f);
}

TEST_CASE("multiple barriers forming harbour walls", "[heightmap][barrier]") {
    // Two separate barrier lines forming an L-shape that, together with the
    // grid edge, enclose a corner pocket of water.
    HeightmapGenerator gen;

    DepthArea water;
    water.minDepth = 8.0;
    water.maxDepth = 12.0;
    water.boundary = {
        {-3.21, 51.43}, {-3.15, 51.43},
        {-3.15, 51.49}, {-3.21, 51.49},
        {-3.21, 51.43}
    };
    gen.setDepthAreas({water});

    HeightmapBounds bounds{-3.20, -3.16, 51.44, 51.48};
    gen.setBounds(bounds);

    // Two barriers forming an L in the top-left quadrant:
    // Vertical barrier from top edge down to mid-grid
    // Horizontal barrier from left edge across to the vertical barrier
    std::vector<std::vector<std::pair<double,double>>> barriers = {
        // Vertical: col ~5, from top to row ~10
        {{51.48, -3.196}, {51.46, -3.196}},
        // Horizontal: row ~10, from left edge to col ~5
        {{51.46, -3.20}, {51.46, -3.196}}
    };
    gen.setBarriers(barriers);

    HeightmapParams params;
    params.resolution = 21;
    params.defaultSeaDepth = 10.0;
    auto grid = gen.generate(params);

    // Top-left corner pocket (row 2, col 1) is enclosed by barriers + edges -> land
    REQUIRE(grid[2][1] >= 0.0f);

    // Bottom-right (row 18, col 18) is open sea -> water
    REQUIRE(grid[18][18] < 0.0f);
}

TEST_CASE("barrier flood-fill with defaultSeaDepth=0 handles IEEE -0.0f", "[heightmap][barrier]") {
    // When defaultSeaDepth=0.0, uncharted water pixels get -0.0f.
    // IEEE 754: -0.0f < 0.0f is FALSE. The <= 0.0f fix ensures these are
    // still treated as water in the flood-fill.
    HeightmapGenerator gen;

    HeightmapBounds bounds{-3.20, -3.16, 51.44, 51.48};
    gen.setBounds(bounds);

    // No depth areas or coastlines -- everything is uncharted.
    // With defaultSeaDepth=0.0, all pixels become -0.0f (or 0.0f).

    // A closed barrier box should still enclose the center.
    std::vector<std::vector<std::pair<double,double>>> barriers = {
        {
            {51.464, -3.184}, {51.464, -3.176},
            {51.456, -3.176}, {51.456, -3.184},
            {51.464, -3.184}
        }
    };
    gen.setBarriers(barriers);

    HeightmapParams params;
    params.resolution = 21;
    params.defaultSeaDepth = 0.0; // triggers -0.0f issue
    auto grid = gen.generate(params);

    // Center inside barrier should be land (flood-fill couldn't reach it)
    REQUIRE(grid[10][10] >= 0.0f);
    REQUIRE(grid[10][10] > 0.0f); // strictly positive = converted to land

    // Corner should remain at sea level (reachable from edge)
    // With defaultSeaDepth=0, this could be 0.0f or -0.0f
    // Key: it was NOT converted to land (positive)
    REQUIRE(grid[0][0] <= 0.0f);
}

TEST_CASE("barrier that does not fully span grid has no enclosed area", "[heightmap][barrier]") {
    // A short barrier in the middle that doesn't touch edges shouldn't enclose anything
    // because the flood-fill can go around it
    HeightmapGenerator gen;

    DepthArea water;
    water.minDepth = 8.0;
    water.maxDepth = 12.0;
    water.boundary = {
        {-3.21, 51.43}, {-3.15, 51.43},
        {-3.15, 51.49}, {-3.21, 51.49},
        {-3.21, 51.43}
    };
    gen.setDepthAreas({water});

    HeightmapBounds bounds{-3.20, -3.16, 51.44, 51.48};
    gen.setBounds(bounds);

    // Short barrier in the center only -- doesn't reach edges
    std::vector<std::vector<std::pair<double,double>>> barriers = {
        {{51.46, -3.185}, {51.46, -3.175}}  // very short, center only
    };
    gen.setBarriers(barriers);

    HeightmapParams params;
    params.resolution = 11;
    auto grid = gen.generate(params);

    // Flood-fill should reach all water pixels since barrier doesn't span full width
    // All non-barrier pixels should remain water (<= 0)
    // Check corners which are definitely reachable
    REQUIRE(grid[0][0] <= 0.0f);   // top-left
    REQUIRE(grid[0][10] <= 0.0f);  // top-right
    REQUIRE(grid[10][0] <= 0.0f);  // bottom-left
    REQUIRE(grid[10][10] <= 0.0f); // bottom-right
}

#endif // WITH_GDAL
