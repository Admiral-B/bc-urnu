#include "BuildingGenerator.hpp"
#include <earcut.hpp>

#include <cmath>
#include <sstream>
#include <iomanip>
#include <array>

// earcut adapter: tell earcut how to read std::pair<float,float>
namespace mapbox {
namespace util {
template <>
struct nth<0, std::pair<float, float>> {
    static float get(const std::pair<float, float>& p) { return p.first; }
};
template <>
struct nth<1, std::pair<float, float>> {
    static float get(const std::pair<float, float>& p) { return p.second; }
};
} // namespace util
} // namespace mapbox

// ---- Material Classification ----

BuildingMaterialClass classifyBuildingMaterial(const std::string& type, float height) {
    if (type == "church" || type == "cathedral" || type == "chapel" ||
        type == "temple" || type == "mosque" || type == "synagogue")
        return BuildingMaterialClass::Stone;
    if (type == "industrial" || type == "warehouse" || type == "manufacture" ||
        type == "factory" || type == "hangar")
        return BuildingMaterialClass::Industrial;
    if (type == "commercial" || type == "office" || type == "retail") {
        if (height > 20.0f) return BuildingMaterialClass::Glass;
        return BuildingMaterialClass::Concrete;
    }
    if (type == "apartments" || type == "hotel") {
        if (height > 30.0f) return BuildingMaterialClass::Glass;
        return BuildingMaterialClass::Concrete;
    }
    if (type == "residential" || type == "house" || type == "detached" ||
        type == "semi" || type == "terrace" || type == "bungalow")
        return BuildingMaterialClass::Brick;
    if (type == "garage" || type == "garages" || type == "shed" ||
        type == "barn" || type == "farm_auxiliary")
        return BuildingMaterialClass::Industrial;
    // Height-based fallback: tall = concrete/glass, short = brick
    if (height > 25.0f) return BuildingMaterialClass::Glass;
    if (height > 15.0f) return BuildingMaterialClass::Concrete;
    return BuildingMaterialClass::Brick;
}

// Pack RGBA into uint32_t (WE vertex color format)
static uint32_t packColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
}

// Get wall and roof vertex colors for a material class with slight random variation
static void getMaterialColors(BuildingMaterialClass matClass, size_t seed,
                               uint32_t& wallColor, uint32_t& roofColor) {
    // Deterministic variation from seed
    uint32_t h = (uint32_t)(seed * 2654435761u); // Knuth multiplicative hash
    int var = (int)(h % 30) - 15; // -15 to +14

    auto clamp = [](int v) -> uint8_t { return (uint8_t)std::max(0, std::min(255, v)); };

    switch (matClass) {
    case BuildingMaterialClass::Brick:
        // Warm brick: reddish-brown to sandy, varied
        wallColor = packColor(clamp(195 + var), clamp(160 + var), clamp(135 + var));
        roofColor = packColor(clamp(120 + var/2), clamp(90 + var/2), clamp(75 + var/2));
        break;
    case BuildingMaterialClass::Concrete:
        // Cool grey concrete
        wallColor = packColor(clamp(200 + var), clamp(200 + var), clamp(205 + var));
        roofColor = packColor(clamp(140 + var/2), clamp(140 + var/2), clamp(145 + var/2));
        break;
    case BuildingMaterialClass::Stone:
        // Warm limestone/sandstone
        wallColor = packColor(clamp(215 + var), clamp(205 + var), clamp(185 + var));
        roofColor = packColor(clamp(130 + var/2), clamp(125 + var/2), clamp(110 + var/2));
        break;
    case BuildingMaterialClass::Industrial:
        // Dark grey-brown, weathered
        wallColor = packColor(clamp(155 + var), clamp(150 + var), clamp(140 + var));
        roofColor = packColor(clamp(110 + var/2), clamp(110 + var/2), clamp(105 + var/2));
        break;
    case BuildingMaterialClass::Glass:
        // Blue-grey glass (high brightness, texture roughness/metalness will do the rest)
        wallColor = packColor(clamp(190 + var), clamp(205 + var), clamp(220 + var));
        roofColor = packColor(clamp(150 + var/2), clamp(150 + var/2), clamp(155 + var/2));
        break;
    default:
        // Neutral white (texture color shows through)
        wallColor = packColor(clamp(230 + var), clamp(225 + var), clamp(220 + var));
        roofColor = packColor(clamp(160 + var/2), clamp(155 + var/2), clamp(150 + var/2));
        break;
    }
}

// ---- BuildingMesh ----

void BuildingMesh::append(const BuildingMesh& other) {
    uint32_t offset = static_cast<uint32_t>(vertexCount());
    positions.insert(positions.end(), other.positions.begin(), other.positions.end());
    normals.insert(normals.end(), other.normals.begin(), other.normals.end());
    uvs.insert(uvs.end(), other.uvs.begin(), other.uvs.end());
    colors.insert(colors.end(), other.colors.begin(), other.colors.end());

    // Keep wall indices [0..wallIndexCount) and roof indices [wallIndexCount..end).
    // We need to splice other's walls into our wall section and other's roofs at end.
    size_t otherWall = std::min(other.wallIndexCount, other.indices.size());

    // Build offset copies of other's wall and roof indices
    std::vector<uint32_t> otherWallIdx(other.indices.begin(), other.indices.begin() + otherWall);
    std::vector<uint32_t> otherRoofIdx(other.indices.begin() + otherWall, other.indices.end());
    for (auto& idx : otherWallIdx) idx += offset;
    for (auto& idx : otherRoofIdx) idx += offset;

    // Batch insert walls at wallIndexCount boundary, roofs at end
    indices.insert(indices.begin() + wallIndexCount, otherWallIdx.begin(), otherWallIdx.end());
    wallIndexCount += otherWallIdx.size();
    indices.insert(indices.end(), otherRoofIdx.begin(), otherRoofIdx.end());
}

std::string BuildingMesh::toOBJ(const std::string& mtlFile,
                                const std::string& wallMtl,
                                const std::string& roofMtl) const {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(4);
    oss << "# Building mesh generated by BuildingGenerator\n";
    oss << "mtllib " << mtlFile << ".mtl\n\n";

    // OBJ is right-handed; BC world coords are left-handed Y-up.
    // Negate Z so the importer's standard Z-flip restores correct positions.
    size_t nv = vertexCount();
    for (size_t i = 0; i < nv; i++) {
        oss << "v " << positions[i * 3] << " " << positions[i * 3 + 1]
            << " " << -positions[i * 3 + 2] << "\n";
    }
    oss << "\n";
    for (size_t i = 0; i < nv; i++) {
        oss << "vn " << normals[i * 3] << " " << normals[i * 3 + 1]
            << " " << -normals[i * 3 + 2] << "\n";
    }
    oss << "\n";
    for (size_t i = 0; i < nv; i++) {
        oss << "vt " << uvs[i * 2] << " " << uvs[i * 2 + 1] << "\n";
    }
    oss << "\n";

    // Wall faces (swap winding b<->c to compensate for Z negation)
    size_t wallTris = wallIndexCount / 3;
    size_t totalTris = triangleCount();
    oss << "usemtl " << wallMtl << "\n";
    for (size_t i = 0; i < wallTris; i++) {
        uint32_t a = indices[i * 3] + 1;
        uint32_t b = indices[i * 3 + 1] + 1;
        uint32_t c = indices[i * 3 + 2] + 1;
        oss << "f " << a << "/" << a << "/" << a
            << " " << c << "/" << c << "/" << c
            << " " << b << "/" << b << "/" << b << "\n";
    }

    // Roof faces
    oss << "\nusemtl " << roofMtl << "\n";
    for (size_t i = wallTris; i < totalTris; i++) {
        uint32_t a = indices[i * 3] + 1;
        uint32_t b = indices[i * 3 + 1] + 1;
        uint32_t c = indices[i * 3 + 2] + 1;
        oss << "f " << a << "/" << a << "/" << a
            << " " << c << "/" << c << "/" << c
            << " " << b << "/" << b << "/" << b << "\n";
    }
    return oss.str();
}

// ---- BuildingGenerator ----

// Map a tiling UV coordinate into an atlas sub-region.
// frac = x - floor(x) gives [0,1), then scale and offset into the cell.
static float atlasUV(float uv, float cellSize, float cellOffset) {
    float frac = uv - std::floor(uv);
    return frac * cellSize + cellOffset;
}

BuildingMesh BuildingGenerator::generate(const BuildingFootprint& fp,
                                          CoordFunc coordFunc,
                                          float groundY,
                                          int wallType,
                                          int roofType,
                                          BuildingMaterialClass materialClass) {
    BuildingMesh mesh;
    if (fp.outline.size() < 3) return mesh;

    // Wall atlas: 2x2 grid (each cell 0.5 x 0.5 in UV space)
    float wallCellU = (wallType % 2) * 0.5f;
    float wallCellV = (wallType / 2) * 0.5f;
    // Roof atlas: 2x2 grid (each cell 0.5 x 0.5 in UV space)
    float roofCellU = (roofType % 2) * 0.5f;
    float roofCellV = (roofType / 2) * 0.5f;

    // Per-building vertex color tint (seed from outline to be deterministic)
    size_t colorSeed = fp.outline.size();
    if (!fp.outline.empty()) {
        colorSeed ^= (size_t)(fp.outline[0].first * 100000.0) ^ (size_t)(fp.outline[0].second * 100000.0);
    }
    uint32_t wallColor, roofColor;
    getMaterialColors(materialClass, colorSeed, wallColor, roofColor);

    float height = fp.height;
    float roofY = groundY + height;

    // Convert lat/lon outline to world X/Z
    std::vector<std::pair<float, float>> worldPoly; // (x, z) in world coords
    worldPoly.reserve(fp.outline.size());
    for (const auto& [lat, lon] : fp.outline) {
        auto [wx, wz] = coordFunc(lat, lon);
        worldPoly.emplace_back(wx, wz);
    }

    // Remove closing duplicate if present
    if (worldPoly.size() > 1 &&
        std::abs(worldPoly.front().first - worldPoly.back().first) < 0.01f &&
        std::abs(worldPoly.front().second - worldPoly.back().second) < 0.01f) {
        worldPoly.pop_back();
    }

    size_t n = worldPoly.size();
    if (n < 3) return mesh;

    // ---- Walls ----
    // For each edge, create a quad (2 triangles)
    // UV: U = cumulative distance along perimeter, V = 0 at ground, 1 at roof
    // Scale UV so one unit = ~3m (one storey)
    float uvScale = 1.0f / 3.0f;

    float cumDist = 0.0f;
    for (size_t i = 0; i < n; i++) {
        size_t j = (i + 1) % n;

        float x0 = worldPoly[i].first, z0 = worldPoly[i].second;
        float x1 = worldPoly[j].first, z1 = worldPoly[j].second;

        float dx = x1 - x0, dz = z1 - z0;
        float edgeLen = std::sqrt(dx * dx + dz * dz);

        // Outward normal (in XZ plane, Y=0)
        float nx = dz, nz = -dx;
        float nLen = std::sqrt(nx * nx + nz * nz);
        if (nLen > 1e-6f) { nx /= nLen; nz /= nLen; }

        // Raw tiling UVs, then remap into wall atlas cell
        float rawU0 = cumDist * uvScale;
        float rawU1 = (cumDist + edgeLen) * uvScale;
        float rawVtop = height * uvScale;

        float u0 = atlasUV(rawU0, 0.5f, wallCellU);
        float u1 = atlasUV(rawU1, 0.5f, wallCellU);
        float v0 = atlasUV(0.0f, 0.5f, wallCellV);
        float v1 = atlasUV(rawVtop, 0.5f, wallCellV);

        uint32_t base = static_cast<uint32_t>(mesh.vertexCount());

        // 4 vertices: bottom-left, bottom-right, top-right, top-left
        mesh.positions.insert(mesh.positions.end(), {x0, groundY, z0});
        mesh.normals.insert(mesh.normals.end(), {nx, 0.0f, nz});
        mesh.uvs.insert(mesh.uvs.end(), {u0, v0});
        mesh.colors.push_back(wallColor);

        mesh.positions.insert(mesh.positions.end(), {x1, groundY, z1});
        mesh.normals.insert(mesh.normals.end(), {nx, 0.0f, nz});
        mesh.uvs.insert(mesh.uvs.end(), {u1, v0});
        mesh.colors.push_back(wallColor);

        mesh.positions.insert(mesh.positions.end(), {x1, roofY, z1});
        mesh.normals.insert(mesh.normals.end(), {nx, 0.0f, nz});
        mesh.uvs.insert(mesh.uvs.end(), {u1, v1});
        mesh.colors.push_back(wallColor);

        mesh.positions.insert(mesh.positions.end(), {x0, roofY, z0});
        mesh.normals.insert(mesh.normals.end(), {nx, 0.0f, nz});
        mesh.uvs.insert(mesh.uvs.end(), {u0, v1});
        mesh.colors.push_back(wallColor);

        // Two triangles (CCW winding when viewed from outside)
        mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2});
        mesh.indices.insert(mesh.indices.end(), {base, base + 2, base + 3});

        cumDist += edgeLen;
    }

    // Record wall index count (everything so far is walls)
    mesh.wallIndexCount = mesh.indices.size();

    // ---- Roof (flat) ----
    // Triangulate the polygon with earcut
    std::vector<std::vector<std::pair<float, float>>> polygon;
    polygon.push_back(worldPoly);

    auto floorIndices = mapbox::earcut<uint32_t>(polygon);

    if (!floorIndices.empty()) {
        uint32_t roofBase = static_cast<uint32_t>(mesh.vertexCount());

        // Add roof vertices (at roofY)
        for (size_t i = 0; i < n; i++) {
            float x = worldPoly[i].first;
            float z = worldPoly[i].second;
            mesh.positions.insert(mesh.positions.end(), {x, roofY, z});
            mesh.normals.insert(mesh.normals.end(), {0.0f, 1.0f, 0.0f});
            // Roof UV: project XZ, remap into roof atlas cell (2x2 grid)
            mesh.uvs.insert(mesh.uvs.end(), {
                atlasUV(x * uvScale, 0.5f, roofCellU),
                atlasUV(z * uvScale, 0.5f, roofCellV)
            });
            mesh.colors.push_back(roofColor);
        }

        for (uint32_t idx : floorIndices) {
            mesh.indices.push_back(roofBase + idx);
        }
    }

    return mesh;
}

BuildingMesh BuildingGenerator::generateBatch(const std::vector<BuildingFootprint>& footprints,
                                               CoordFunc coordFunc,
                                               float groundY) {
    BuildingMesh batch;
    int idx = 0;
    for (const auto& fp : footprints) {
        auto matClass = classifyBuildingMaterial(fp.type, fp.height);
        BuildingMesh single = generate(fp, coordFunc, groundY, idx % 4, (idx * 3 + 1) % 4, matClass);
        if (!single.empty()) {
            batch.append(single);
        }
        idx++;
    }
    return batch;
}
