#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "BuildingGenerator.hpp"

using Catch::Approx;

// Simple identity coord func: lat -> x, lon -> z (no projection)
static std::pair<float, float> identityCoord(double lat, double lon) {
    return {static_cast<float>(lat), static_cast<float>(lon)};
}

// ── BuildingMesh basics ─────────────────────────────────────────────────────

TEST_CASE("BuildingMesh default is empty", "[building]") {
    BuildingMesh m;
    REQUIRE(m.empty());
    REQUIRE(m.vertexCount() == 0);
    REQUIRE(m.triangleCount() == 0);
}

TEST_CASE("BuildingMesh append merges correctly", "[building]") {
    BuildingMesh a, b;

    // Mesh A: single triangle
    a.positions = {0, 0, 0, 1, 0, 0, 0, 0, 1};
    a.normals = {0, 1, 0, 0, 1, 0, 0, 1, 0};
    a.uvs = {0, 0, 1, 0, 0, 1};
    a.indices = {0, 1, 2};

    // Mesh B: single triangle
    b.positions = {2, 0, 0, 3, 0, 0, 2, 0, 1};
    b.normals = {0, 1, 0, 0, 1, 0, 0, 1, 0};
    b.uvs = {0, 0, 1, 0, 0, 1};
    b.indices = {0, 1, 2};

    a.append(b);

    REQUIRE(a.vertexCount() == 6);
    REQUIRE(a.triangleCount() == 2);
    // Second triangle indices should be offset by 3
    REQUIRE(a.indices[3] == 3);
    REQUIRE(a.indices[4] == 4);
    REQUIRE(a.indices[5] == 5);
}

// ── BuildingGenerator::generate ─────────────────────────────────────────────

TEST_CASE("generate rejects degenerate footprint", "[building]") {
    BuildingFootprint fp;
    fp.outline = {{0, 0}, {1, 0}}; // only 2 points
    fp.height = 10.0f;

    BuildingMesh mesh = BuildingGenerator::generate(fp, identityCoord);
    REQUIRE(mesh.empty());
}

TEST_CASE("generate produces mesh for triangle footprint", "[building]") {
    BuildingFootprint fp;
    fp.outline = {{0, 0}, {10, 0}, {5, 10}};
    fp.height = 6.0f;
    fp.type = "residential";

    BuildingMesh mesh = BuildingGenerator::generate(fp, identityCoord);

    REQUIRE_FALSE(mesh.empty());
    REQUIRE(mesh.vertexCount() > 0);
    REQUIRE(mesh.triangleCount() > 0);

    // Walls: 3 edges * 4 verts = 12 wall verts
    // Roof: 3 verts
    // Total: 15 verts minimum
    REQUIRE(mesh.vertexCount() >= 15);
}

TEST_CASE("generate produces mesh for rectangular footprint", "[building]") {
    BuildingFootprint fp;
    fp.outline = {{0, 0}, {20, 0}, {20, 10}, {0, 10}};
    fp.height = 9.0f;
    fp.type = "commercial";

    BuildingMesh mesh = BuildingGenerator::generate(fp, identityCoord);

    REQUIRE_FALSE(mesh.empty());
    // Walls: 4 edges * 4 verts = 16 wall verts
    // Roof: 4 verts
    // Total: 20
    REQUIRE(mesh.vertexCount() >= 20);

    // Wall triangles: 4 edges * 2 tris = 8
    // Roof triangles: 2 (rectangle -> 2 triangles)
    // Total: 10
    REQUIRE(mesh.triangleCount() >= 10);
}

TEST_CASE("generate respects groundY offset", "[building]") {
    BuildingFootprint fp;
    fp.outline = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};
    fp.height = 5.0f;

    float groundY = 100.0f;
    BuildingMesh mesh = BuildingGenerator::generate(fp, identityCoord, groundY);

    REQUIRE_FALSE(mesh.empty());

    // All Y positions should be >= groundY (100) and <= groundY + height (105)
    for (size_t i = 0; i < mesh.vertexCount(); i++) {
        float y = mesh.positions[i * 3 + 1];
        REQUIRE(y >= Approx(groundY).margin(0.01f));
        REQUIRE(y <= Approx(groundY + fp.height).margin(0.01f));
    }
}

TEST_CASE("generate removes closing duplicate vertex", "[building]") {
    BuildingFootprint fp;
    // Closed ring: first == last
    fp.outline = {{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}};
    fp.height = 6.0f;

    BuildingMesh mesh = BuildingGenerator::generate(fp, identityCoord);

    // Should produce same result as 4-vertex version (closing dup removed)
    BuildingFootprint fp4;
    fp4.outline = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};
    fp4.height = 6.0f;

    BuildingMesh mesh4 = BuildingGenerator::generate(fp4, identityCoord);

    REQUIRE(mesh.vertexCount() == mesh4.vertexCount());
    REQUIRE(mesh.triangleCount() == mesh4.triangleCount());
}

// ── BuildingGenerator::generateBatch ────────────────────────────────────────

TEST_CASE("generateBatch empty input produces empty mesh", "[building]") {
    std::vector<BuildingFootprint> fps;
    BuildingMesh mesh = BuildingGenerator::generateBatch(fps, identityCoord);
    REQUIRE(mesh.empty());
}

TEST_CASE("generateBatch merges multiple buildings", "[building]") {
    BuildingFootprint fp1;
    fp1.outline = {{0, 0}, {5, 0}, {5, 5}, {0, 5}};
    fp1.height = 6.0f;

    BuildingFootprint fp2;
    fp2.outline = {{20, 0}, {30, 0}, {30, 10}, {20, 10}};
    fp2.height = 12.0f;

    auto single1 = BuildingGenerator::generate(fp1, identityCoord);
    auto single2 = BuildingGenerator::generate(fp2, identityCoord);
    auto batch = BuildingGenerator::generateBatch({fp1, fp2}, identityCoord);

    REQUIRE(batch.vertexCount() == single1.vertexCount() + single2.vertexCount());
    REQUIRE(batch.triangleCount() == single1.triangleCount() + single2.triangleCount());
}

// ── OBJ export ──────────────────────────────────────────────────────────────

TEST_CASE("toOBJ produces valid wavefront format", "[building]") {
    BuildingFootprint fp;
    fp.outline = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};
    fp.height = 6.0f;

    BuildingMesh mesh = BuildingGenerator::generate(fp, identityCoord);
    std::string obj = mesh.toOBJ("test_mtl");

    REQUIRE(obj.find("mtllib test_mtl.mtl") != std::string::npos);
    // toOBJ uses separate wall/roof material names (defaults: building_wall, building_roof)
    REQUIRE(obj.find("usemtl building_wall") != std::string::npos);
    REQUIRE(obj.find("v ") != std::string::npos);
    REQUIRE(obj.find("vn ") != std::string::npos);
    REQUIRE(obj.find("vt ") != std::string::npos);
    REQUIRE(obj.find("f ") != std::string::npos);
}

// ── OSMBuildingReader height estimation ─────────────────────────────────────

TEST_CASE("BuildingFootprint default height", "[building]") {
    BuildingFootprint fp;
    REQUIRE(fp.height == Approx(9.0f));
}
