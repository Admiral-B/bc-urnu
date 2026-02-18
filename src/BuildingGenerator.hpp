#pragma once

#include "editor/OSMBuildingReader.hpp"

#include <vector>
#include <cstdint>
#include <functional>
#include <string>

// Generated mesh data for a single building or a batch of buildings.
// Engine-agnostic: just vertex/index arrays that can be fed to WE or written as .obj.
struct BuildingMesh {
    std::vector<float> positions; // x,y,z interleaved
    std::vector<float> normals;   // nx,ny,nz interleaved
    std::vector<float> uvs;       // u,v interleaved
    std::vector<uint32_t> indices; // triangle indices
    size_t wallIndexCount = 0;    // indices [0..wallIndexCount) = walls, rest = roofs

    bool empty() const { return positions.empty(); }
    size_t vertexCount() const { return positions.size() / 3; }
    size_t triangleCount() const { return indices.size() / 3; }

    // Append another mesh (for batching)
    void append(const BuildingMesh& other);

    // Write as Wavefront .obj with separate wall/roof materials
    // mtlFile is the .mtl filename, wallMtl/roofMtl are the material names within it
    std::string toOBJ(const std::string& mtlFile = "building",
                      const std::string& wallMtl = "building_wall",
                      const std::string& roofMtl = "building_roof") const;
};

class BuildingGenerator {
public:
    // Coordinate converter: (lat, lon) -> (worldX, worldZ)
    using CoordFunc = std::function<std::pair<float, float>(double lat, double lon)>;

    // Generate mesh for a single building footprint.
    // coordFunc converts lat/lon to world X/Z.
    // groundY is the Y level of the terrain at this building (0 if flat).
    static BuildingMesh generate(const BuildingFootprint& fp,
                                  CoordFunc coordFunc,
                                  float groundY = 0.0f);

    // Generate and merge meshes for a batch of buildings (single draw call).
    static BuildingMesh generateBatch(const std::vector<BuildingFootprint>& footprints,
                                       CoordFunc coordFunc,
                                       float groundY = 0.0f);
};
