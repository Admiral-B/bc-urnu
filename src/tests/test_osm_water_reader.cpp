#include <catch2/catch_test_macros.hpp>

// OSMWaterReader doesn't depend on GDAL, but lives in editor/ directory.
// We test parseResponse() directly with synthetic JSON.
#include "editor/OSMWaterReader.hpp"

TEST_CASE("parseResponse extracts dam barriers", "[osmwater][barrier]") {
    OSMWaterReader reader;

    // Simulates Overpass response with a waterway=dam way
    std::string json = R"({
        "elements": [
            {
                "type": "way",
                "id": 12345,
                "tags": {
                    "waterway": "dam",
                    "name": "Cardiff Bay Barrage"
                },
                "geometry": [
                    {"lat": 51.4450, "lon": -3.1700},
                    {"lat": 51.4455, "lon": -3.1680},
                    {"lat": 51.4460, "lon": -3.1660}
                ]
            }
        ]
    })";

    REQUIRE(reader.parseResponse(json));

    // Should have 1 barrier, 0 water areas
    REQUIRE(reader.getBarriers().size() == 1);
    REQUIRE(reader.getWaterAreas().empty());

    // Check barrier geometry
    const auto& barrier = reader.getBarriers()[0];
    REQUIRE(barrier.size() == 3);
    REQUIRE(barrier[0].first == 51.4450);  // lat
    REQUIRE(barrier[0].second == -3.1700); // lon
}

TEST_CASE("parseResponse extracts breakwater barriers", "[osmwater][barrier]") {
    OSMWaterReader reader;

    std::string json = R"({
        "elements": [
            {
                "type": "way",
                "id": 67890,
                "tags": {
                    "man_made": "breakwater",
                    "area": "yes"
                },
                "geometry": [
                    {"lat": 51.440, "lon": -3.180},
                    {"lat": 51.441, "lon": -3.178},
                    {"lat": 51.442, "lon": -3.176},
                    {"lat": 51.441, "lon": -3.174},
                    {"lat": 51.440, "lon": -3.180}
                ]
            }
        ]
    })";

    REQUIRE(reader.parseResponse(json));
    REQUIRE(reader.getBarriers().size() == 1);
    REQUIRE(reader.getWaterAreas().empty());

    const auto& barrier = reader.getBarriers()[0];
    REQUIRE(barrier.size() == 5); // closed polygon stored as polyline
}

TEST_CASE("parseResponse separates water and barriers", "[osmwater][barrier]") {
    OSMWaterReader reader;

    // Mix of water polygon and barrier way
    std::string json = R"({
        "elements": [
            {
                "type": "way",
                "id": 100,
                "tags": {"natural": "water", "water": "lake"},
                "geometry": [
                    {"lat": 51.45, "lon": -3.18},
                    {"lat": 51.46, "lon": -3.18},
                    {"lat": 51.46, "lon": -3.16},
                    {"lat": 51.45, "lon": -3.16},
                    {"lat": 51.45, "lon": -3.18}
                ]
            },
            {
                "type": "way",
                "id": 200,
                "tags": {"waterway": "dam"},
                "geometry": [
                    {"lat": 51.45, "lon": -3.17},
                    {"lat": 51.46, "lon": -3.17}
                ]
            },
            {
                "type": "way",
                "id": 300,
                "tags": {"man_made": "breakwater"},
                "geometry": [
                    {"lat": 51.44, "lon": -3.19},
                    {"lat": 51.44, "lon": -3.17}
                ]
            }
        ]
    })";

    REQUIRE(reader.parseResponse(json));
    REQUIRE(reader.getWaterAreas().size() == 1);
    REQUIRE(reader.getBarriers().size() == 2);

    // Water area should be classified as lake
    REQUIRE(reader.getWaterAreas()[0].type == "lake");
}

TEST_CASE("parseResponse handles closed polygon barrier (Cardiff Bay shape)", "[osmwater][barrier]") {
    OSMWaterReader reader;

    // Simulates the actual Cardiff Bay Barrage (OSM way 1287700755):
    // a closed polygon (first == last node) with many intermediate nodes.
    std::string json = R"({
        "elements": [
            {
                "type": "way",
                "id": 1287700755,
                "tags": {
                    "waterway": "dam",
                    "name": "Cardiff Bay Barrage"
                },
                "geometry": [
                    {"lat": 51.4470637, "lon": -3.1659207},
                    {"lat": 51.4471315, "lon": -3.1658517},
                    {"lat": 51.4473200, "lon": -3.1655800},
                    {"lat": 51.4475100, "lon": -3.1652000},
                    {"lat": 51.4477000, "lon": -3.1648000},
                    {"lat": 51.4479000, "lon": -3.1644000},
                    {"lat": 51.4481000, "lon": -3.1640000},
                    {"lat": 51.4483000, "lon": -3.1636000},
                    {"lat": 51.4484500, "lon": -3.1632000},
                    {"lat": 51.4485200, "lon": -3.1628000},
                    {"lat": 51.4485000, "lon": -3.1624000},
                    {"lat": 51.4483500, "lon": -3.1620000},
                    {"lat": 51.4481000, "lon": -3.1618000},
                    {"lat": 51.4478000, "lon": -3.1617000},
                    {"lat": 51.4475000, "lon": -3.1618000},
                    {"lat": 51.4472000, "lon": -3.1621000},
                    {"lat": 51.4470637, "lon": -3.1659207}
                ]
            }
        ]
    })";

    REQUIRE(reader.parseResponse(json));
    REQUIRE(reader.getBarriers().size() == 1);

    const auto& barrier = reader.getBarriers()[0];
    REQUIRE(barrier.size() == 17);

    // First and last points should be identical (closed polygon)
    REQUIRE(barrier.front().first == barrier.back().first);
    REQUIRE(barrier.front().second == barrier.back().second);
}

TEST_CASE("parseResponse handles multiple barriers in one response", "[osmwater][barrier]") {
    OSMWaterReader reader;

    // Multiple dams and breakwaters in one area (Cardiff Bay area returns 3 ways)
    std::string json = R"({
        "elements": [
            {
                "type": "way",
                "id": 301969049,
                "tags": {"waterway": "dam", "material": "rock"},
                "geometry": [
                    {"lat": 51.4614, "lon": -3.1610},
                    {"lat": 51.4613, "lon": -3.1607},
                    {"lat": 51.4614, "lon": -3.1606},
                    {"lat": 51.4615, "lon": -3.1609},
                    {"lat": 51.4614, "lon": -3.1610}
                ]
            },
            {
                "type": "way",
                "id": 317753008,
                "tags": {"waterway": "dam"},
                "geometry": [
                    {"lat": 51.4477, "lon": -3.1838},
                    {"lat": 51.4476, "lon": -3.1838},
                    {"lat": 51.4481, "lon": -3.1818}
                ]
            },
            {
                "type": "way",
                "id": 1287700755,
                "tags": {"waterway": "dam", "name": "Cardiff Bay Barrage"},
                "geometry": [
                    {"lat": 51.4471, "lon": -3.1659},
                    {"lat": 51.4475, "lon": -3.1652},
                    {"lat": 51.4480, "lon": -3.1645},
                    {"lat": 51.4471, "lon": -3.1659}
                ]
            }
        ]
    })";

    REQUIRE(reader.parseResponse(json));
    REQUIRE(reader.getBarriers().size() == 3);
    REQUIRE(reader.getWaterAreas().empty());
}

TEST_CASE("parseResponse handles empty elements array", "[osmwater]") {
    OSMWaterReader reader;

    // Overpass returns valid JSON but no elements (empty area or timeout fallback)
    std::string json = R"({"elements": []})";
    REQUIRE(reader.parseResponse(json));
    REQUIRE(reader.getBarriers().empty());
    REQUIRE(reader.getWaterAreas().empty());
}

TEST_CASE("parseResponse handles malformed JSON gracefully", "[osmwater]") {
    OSMWaterReader reader;

    // Truncated/malformed JSON (e.g. from a network timeout)
    std::string json = R"({"elements": [{"type": "way", "id": 123, "tags": {"waterway": )";
    REQUIRE_FALSE(reader.parseResponse(json));
}

TEST_CASE("parseResponse ignores barrier with fewer than 2 points", "[osmwater][barrier]") {
    OSMWaterReader reader;

    std::string json = R"({
        "elements": [
            {
                "type": "way",
                "id": 999,
                "tags": {"waterway": "dam"},
                "geometry": [
                    {"lat": 51.45, "lon": -3.17}
                ]
            }
        ]
    })";

    REQUIRE(reader.parseResponse(json));
    REQUIRE(reader.getBarriers().empty());
}
