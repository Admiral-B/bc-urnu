/*   Bridge Command 5.0 Ship Simulator
     Copyright (C) 2026 James Packer

     This program is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License version 2 as
     published by the Free Software Foundation */

#ifdef WITH_WICKED_ENGINE

#include "WickedTerrainNode.hpp"

// stb_image implementation is already in WickedEngine_Windows.lib -- just include the header
#include "../../libs/stb/stb_image.h"

#include <cmath>
#include <fstream>
#include <iostream>
#include <algorithm>

using namespace wi::graphics;
using namespace wi::scene;
using namespace wi::ecs;

namespace bc { namespace graphics { namespace wicked {

static constexpr float DEG_TO_RAD = 3.14159265358979f / 180.0f;
static constexpr float EARTH_RADIUS = 6371000.0f; // meters

WickedTerrainNode::WickedTerrainNode(Scene* scene)
    : weScene(scene) {}

WickedTerrainNode::~WickedTerrainNode() {
    remove();
}

bool WickedTerrainNode::loadFromConfig(const TerrainTileConfig& config,
                                         float refLongitude, float refLatitude) {
    // Calculate world-space dimensions from geo coordinates
    // Use mid-latitude cosine to match BC's Terrain.cpp coordinate conversion
    float midLat = config.latitude + config.latExtent / 2.0f;
    float cosLat = std::cos(midLat * DEG_TO_RAD);
    worldWidth_ = config.lonExtent * DEG_TO_RAD * EARTH_RADIUS * cosLat;
    worldDepth_ = config.latExtent * DEG_TO_RAD * EARTH_RADIUS;
    maxHeight_ = config.maxHeight;
    seaMaxDepth_ = config.seaMaxDepth;

    // Calculate position offset relative to reference point
    float offsetX = (config.longitude - refLongitude) * DEG_TO_RAD * EARTH_RADIUS * cosLat;
    float offsetZ = (config.latitude - refLatitude) * DEG_TO_RAD * EARTH_RADIUS;
    position_ = {offsetX, -seaMaxDepth_, offsetZ};

    // Determine heightmap format and load
    std::string ext;
    {
        size_t dotPos = config.heightmapPath.find_last_of('.');
        if (dotPos != std::string::npos) {
            ext = config.heightmapPath.substr(dotPos);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        }
    }

    if (ext == ".f32") {
        if (!loadHeightmapF32(config.heightmapPath, config.heightmapRows, config.heightmapCols))
            return false;
    } else {
        // PNG or other image format
        if (!loadHeightmapPNG(config.heightmapPath, config))
            return false;
    }

    return createTerrainMesh(config.texturePath);
}

bool WickedTerrainNode::loadFromHeightData(const std::vector<std::vector<float>>& heights,
                                             float maxHeight, float seaMaxDepth,
                                             float worldWidth, float worldDepth,
                                             const std::string& texturePath) {
    heightData_ = heights;
    maxHeight_ = maxHeight;
    seaMaxDepth_ = seaMaxDepth;
    worldWidth_ = worldWidth;
    worldDepth_ = worldDepth;
    return createTerrainMesh(texturePath);
}

bool WickedTerrainNode::loadHeightmapPNG(const std::string& path,
                                           const TerrainTileConfig& config) {
    int imgW = 0, imgH = 0, imgChannels = 0;

    if (config.usesRGB) {
        // RGB-encoded heightmap: Height = R*256 + G + B/256 - 32768 (absolute meters)
        unsigned char* pixels = stbi_load(path.c_str(), &imgW, &imgH, &imgChannels, 3);
        if (!pixels) {
            std::cerr << "WickedTerrainNode: Failed to load heightmap image: " << path
                      << " (" << stbi_failure_reason() << ")" << std::endl;
            return false;
        }

        // Heights are absolute meters (sea level = 0), so position terrain at Y=0
        position_.y = 0.0f;

        heightData_.resize(imgH, std::vector<float>(imgW));
        for (int r = 0; r < imgH; r++) {
            int srcRow = imgH - 1 - r; // flip: image top (north) -> high Z in world
            for (int c = 0; c < imgW; c++) {
                int idx = (srcRow * imgW + c) * 3;
                float R = static_cast<float>(pixels[idx + 0]);
                float G = static_cast<float>(pixels[idx + 1]);
                float B = static_cast<float>(pixels[idx + 2]);
                heightData_[r][c] = R * 256.0f + G + B / 256.0f - 32768.0f;
            }
        }

        stbi_image_free(pixels);
        std::cout << "WickedTerrainNode: Loaded RGB heightmap (" << imgW << "x" << imgH
                  << ") from " << path << " (absolute meters)" << std::endl;
    } else {
        // Legacy grayscale heightmap: pixel 0..255 maps to 0..(maxHeight+seaMaxDepth)
        // Terrain positioned at Y = -seaMaxDepth
        unsigned char* pixels = stbi_load(path.c_str(), &imgW, &imgH, &imgChannels, 1);
        if (!pixels) {
            std::cerr << "WickedTerrainNode: Failed to load heightmap image: " << path
                      << " (" << stbi_failure_reason() << ")" << std::endl;
            return false;
        }

        float heightRange = maxHeight_ + seaMaxDepth_;
        float invMaxPixel = heightRange / 255.0f;

        heightData_.resize(imgH, std::vector<float>(imgW));
        for (int r = 0; r < imgH; r++) {
            int srcRow = imgH - 1 - r;
            for (int c = 0; c < imgW; c++) {
                float pixelValue = static_cast<float>(pixels[srcRow * imgW + c]);
                heightData_[r][c] = pixelValue * invMaxPixel;
            }
        }

        stbi_image_free(pixels);
        std::cout << "WickedTerrainNode: Loaded heightmap (" << imgW << "x" << imgH
                  << ", " << imgChannels << "ch) from " << path
                  << " heightRange=" << heightRange << "m" << std::endl;
    }

    return true;
}

bool WickedTerrainNode::loadHeightmapF32(const std::string& path, int rows, int cols) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "WickedTerrainNode: Failed to open F32 heightmap: " << path << std::endl;
        return false;
    }

    if (rows <= 0 || cols <= 0) {
        // Try to infer dimensions from file size
        file.seekg(0, std::ios::end);
        size_t fileSize = file.tellg();
        file.seekg(0);
        size_t numFloats = fileSize / sizeof(float);
        int dim = static_cast<int>(std::sqrt(static_cast<double>(numFloats)));
        if (dim * dim != (int)numFloats) {
            std::cerr << "WickedTerrainNode: Cannot infer F32 heightmap dimensions" << std::endl;
            return false;
        }
        rows = cols = dim;
    }

    heightData_.resize(rows, std::vector<float>(cols));
    for (int r = 0; r < rows; r++) {
        file.read(reinterpret_cast<char*>(heightData_[r].data()), cols * sizeof(float));
    }

    std::cout << "WickedTerrainNode: Loaded F32 heightmap (" << rows << "x" << cols
              << ") from " << path << std::endl;
    return true;
}

bool WickedTerrainNode::createTerrainMesh(const std::string& texturePath) {
    if (heightData_.empty()) return false;

    int rows = static_cast<int>(heightData_.size());
    int cols = static_cast<int>(heightData_[0].size());

    // Subsample large heightmaps for performance (max 512x512 mesh)
    int step = 1;
    while (rows / step > 512 || cols / step > 512) step *= 2;
    int meshRows = rows / step;
    int meshCols = cols / step;

    float cellWidth = worldWidth_ / meshCols;
    float cellDepth = worldDepth_ / meshRows;

    // Create mesh entity
    rootEntity_ = weScene->Entity_CreateObject("BC_Terrain");
    Entity meshEntity = weScene->Entity_CreateMesh("BC_Terrain_mesh");
    weScene->Component_Attach(meshEntity, rootEntity_);

    auto* object = weScene->objects.GetComponent(rootEntity_);
    auto* mesh = weScene->meshes.GetComponent(meshEntity);
    if (!object || !mesh) return false;

    object->meshID = meshEntity;

    // Create material
    Entity matEntity = weScene->Entity_CreateMaterial("BC_TerrainMat");
    weScene->Component_Attach(matEntity, rootEntity_);
    auto* material = weScene->materials.GetComponent(matEntity);
    if (material) {
        material->roughness = 0.95f;   // terrain is rough earth/grass, not shiny
        material->metalness = 0.0f;    // non-metallic
        material->SetDoubleSided(true);  // visible from both sides
        if (!texturePath.empty()) {
            // Normalize path separators for WE
            std::string normalizedPath = texturePath;
            std::replace(normalizedPath.begin(), normalizedPath.end(), '\\', '/');
            material->textures[MaterialComponent::BASECOLORMAP].name = normalizedPath;
            // Explicitly pre-load texture (matching how ship textures are loaded)
            if (wi::helper::FileExists(normalizedPath)) {
                material->textures[MaterialComponent::BASECOLORMAP].resource =
                    wi::resourcemanager::Load(normalizedPath);
                std::cout << "WickedTerrainNode: texture loaded: " << normalizedPath << std::endl;
            } else {
                std::cerr << "WickedTerrainNode: texture NOT found: " << normalizedPath << std::endl;
            }
        }
        material->CreateRenderData();
    }

    // Build terrain mesh vertices and indices
    mesh->subsets.push_back(MeshComponent::MeshSubset());
    mesh->subsets.back().materialID = matEntity;
    mesh->subsets.back().indexOffset = 0;

    for (int r = 0; r < meshRows; r++) {
        for (int c = 0; c < meshCols; c++) {
            int srcR = std::min(r * step, rows - 1);
            int srcC = std::min(c * step, cols - 1);
            float height = heightData_[srcR][srcC];

            DirectX::XMFLOAT3 pos(
                c * cellWidth,
                height,
                r * cellDepth
            );

            // Compute normal from neighboring heights
            float hL = (c > 0) ? heightData_[srcR][std::max(0, srcC - step)] : height;
            float hR = (c < meshCols - 1) ? heightData_[srcR][std::min(cols - 1, srcC + step)] : height;
            float hD = (r > 0) ? heightData_[std::max(0, srcR - step)][srcC] : height;
            float hU = (r < meshRows - 1) ? heightData_[std::min(rows - 1, srcR + step)][srcC] : height;

            DirectX::XMFLOAT3 nor(
                (hL - hR) / (2.0f * cellWidth),
                1.0f,
                (hD - hU) / (2.0f * cellDepth)
            );
            // Normalize
            float len = std::sqrt(nor.x * nor.x + nor.y * nor.y + nor.z * nor.z);
            if (len > 0) { nor.x /= len; nor.y /= len; nor.z /= len; }

            DirectX::XMFLOAT2 uv(
                static_cast<float>(c) / (meshCols - 1),
                1.0f - static_cast<float>(r) / (meshRows - 1) // flip V: row 0=south→V=1, last=north→V=0
            );

            // Both BC and WE are left-handed Y-up, no Z flip needed

            mesh->vertex_positions.push_back(pos);
            mesh->vertex_normals.push_back(nor);
            mesh->vertex_uvset_0.push_back(uv);
        }
    }

    // Generate triangle indices
    for (int r = 0; r < meshRows - 1; r++) {
        for (int c = 0; c < meshCols - 1; c++) {
            uint32_t tl = r * meshCols + c;
            uint32_t tr = tl + 1;
            uint32_t bl = (r + 1) * meshCols + c;
            uint32_t br = bl + 1;

            // Two triangles per quad -- DX left-handed, CW = front face from above
            mesh->indices.push_back(tl);
            mesh->indices.push_back(tr);
            mesh->indices.push_back(bl);

            mesh->indices.push_back(tr);
            mesh->indices.push_back(br);
            mesh->indices.push_back(bl);
        }
    }

    mesh->subsets.back().indexCount = static_cast<uint32_t>(mesh->indices.size());
    mesh->CreateRenderData();

    // Position the terrain
    auto* transform = weScene->transforms.GetComponent(rootEntity_);
    if (transform) {
        transform->Translate(DirectX::XMFLOAT3(position_.x, position_.y, position_.z));
        transform->UpdateTransform();
    }

    std::cout << "WickedTerrainNode: Created terrain mesh ("
              << meshCols << "x" << meshRows << " vertices, "
              << mesh->indices.size() / 3 << " triangles)" << std::endl;
    return true;
}

float WickedTerrainNode::getHeightAt(float worldX, float worldZ) const {
    if (heightData_.empty() || worldWidth_ <= 0 || worldDepth_ <= 0) return 0;

    int rows = static_cast<int>(heightData_.size());
    int cols = static_cast<int>(heightData_[0].size());

    // Convert world position to grid coordinates
    float localX = worldX - position_.x;
    float localZ = worldZ - position_.z;
    float gridX = (localX / worldWidth_) * (cols - 1);
    float gridZ = (localZ / worldDepth_) * (rows - 1);

    // Clamp to grid bounds
    gridX = std::max(0.0f, std::min(gridX, static_cast<float>(cols - 2)));
    gridZ = std::max(0.0f, std::min(gridZ, static_cast<float>(rows - 2)));

    // Bilinear interpolation
    int ix = static_cast<int>(gridX);
    int iz = static_cast<int>(gridZ);
    float fx = gridX - ix;
    float fz = gridZ - iz;

    float h00 = heightData_[iz][ix];
    float h10 = heightData_[iz][ix + 1];
    float h01 = heightData_[iz + 1][ix];
    float h11 = heightData_[iz + 1][ix + 1];

    return h00 * (1 - fx) * (1 - fz) +
           h10 * fx * (1 - fz) +
           h01 * (1 - fx) * fz +
           h11 * fx * fz;
}

void WickedTerrainNode::setPosition(const Vec3& pos) {
    position_ = pos;
    if (rootEntity_ != INVALID_ENTITY) {
        auto* transform = weScene->transforms.GetComponent(rootEntity_);
        if (transform) {
            transform->ClearTransform();
            transform->Translate(DirectX::XMFLOAT3(pos.x, pos.y, pos.z));
            transform->UpdateTransform();
        }
    }
}

Vec3 WickedTerrainNode::getPosition() const {
    return position_;
}

void WickedTerrainNode::setRotation(const Vec3& rot) {
    // Terrain rotation is rarely used but supported
    if (rootEntity_ != INVALID_ENTITY) {
        auto* transform = weScene->transforms.GetComponent(rootEntity_);
        if (transform) {
            float degToRad = 3.14159265358979f / 180.0f;
            transform->ClearTransform();
            transform->Translate(DirectX::XMFLOAT3(position_.x, position_.y, position_.z));
            transform->RotateRollPitchYaw(DirectX::XMFLOAT3(rot.x * degToRad, rot.y * degToRad, rot.z * degToRad));
            transform->UpdateTransform();
        }
    }
}

void WickedTerrainNode::setScale(const Vec3& scale) {
    scale_ = scale;
    if (rootEntity_ != INVALID_ENTITY) {
        auto* transform = weScene->transforms.GetComponent(rootEntity_);
        if (transform) {
            transform->scale_local = DirectX::XMFLOAT3(scale.x, scale.y, scale.z);
            transform->SetDirty();
        }
    }
}

void WickedTerrainNode::setVisible(bool visible) {
    visible_ = visible;
    if (rootEntity_ != INVALID_ENTITY) {
        auto* object = weScene->objects.GetComponent(rootEntity_);
        if (object) object->SetRenderable(visible);
    }
}

void WickedTerrainNode::remove() {
    if (rootEntity_ != INVALID_ENTITY && weScene) {
        weScene->Entity_Remove(rootEntity_);
        rootEntity_ = INVALID_ENTITY;
    }
    heightData_.clear();
}

}}} // namespace bc::graphics::wicked

#endif // WITH_WICKED_ENGINE
