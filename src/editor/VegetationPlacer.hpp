#pragma once

#include "OSMLandUseReader.hpp"
#include <vector>
#include <string>
#include <cstdint>
#include <functional>
#include <utility>

// Tree species for multi-atlas rendering
enum class TreeSpecies : uint8_t {
    Deciduous = 0,  // Oak/beech/elm -- broadleaf, green canopy
    Conifer   = 1,  // Pine/spruce -- tall narrow evergreen
    Shrub     = 2,  // Low bushes/hedgerows -- wide, short
    Palm      = 3,  // Tropical -- only at low latitudes
    COUNT
};

struct TreePlacement {
    double longitude;
    double latitude;
    float height;       // terrain height at placement (metres)
    float scale;        // random size variation (0.6 - 1.4)
    float rotation;     // random rotation (degrees)
    TreeSpecies species;
};

class VegetationPlacer {
public:
    using ProgressCallback = std::function<void(const std::string&)>;

    // Generate tree placements from land use grid and heightmap.
    // landUseGrid: uint8_t[resolution*resolution], row-major, row 0 = north
    // heightGrid: float[resolution*resolution], metres (negative = water)
    // Geo bounds define the world area.
    // midLatitude used for species selection (palm > 35 deg only at low lat).
    // maxTrees caps output for performance.
    void generate(const uint8_t* landUseGrid,
                  const float* heightGrid,
                  int resolution,
                  double minLat, double maxLat,
                  double minLon, double maxLon,
                  int maxTrees = 15000,
                  ProgressCallback progress = nullptr);

    const std::vector<TreePlacement>& getTrees() const { return trees; }

    // Write trees.ini in BC format.
    // Species is encoded as Species(N)=0..3 for multi-atlas rendering.
    std::string generateTreesIni(const std::string& textureFile = "tree_billboard.png") const;

    // Generate a procedural 2x2 atlas texture (RGBA, 4 channels).
    // Each quadrant is cellSize x cellSize pixels.
    // Returns RGBA pixel data (width = height = cellSize*2).
    static std::vector<uint8_t> generateAtlasTexture(int cellSize = 256);

    // Write RGBA atlas texture as PNG to disk.
    static bool writeAtlasPNG(const std::string& path, const std::vector<uint8_t>& rgbaData,
                              int width, int height);

private:
    std::vector<TreePlacement> trees;

    // Density (trees per hectare) for each land use type
    static float densityForLandUse(LandUseType type);

    // Species selection based on land use and latitude
    static TreeSpecies selectSpecies(LandUseType type, double latitude, uint32_t hash);
};
