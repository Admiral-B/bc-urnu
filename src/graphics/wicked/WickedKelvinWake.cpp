/*   Bridge Command 5.0 Ship Simulator
     Copyright (C) 2026 James Packer

     This program is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License version 2 as
     published by the Free Software Foundation */

#ifdef WITH_WICKED_ENGINE

#include "WickedKelvinWake.hpp"
#include "WickedEngine.h"
#include <cmath>
#include <algorithm>
#include <iostream>

using namespace wi::ecs;
using namespace wi::scene;

namespace bc { namespace graphics { namespace wicked {

WickedKelvinWake::WickedKelvinWake() = default;

WickedKelvinWake::~WickedKelvinWake() {
    shutdown();
}

void WickedKelvinWake::init(Scene* scene, const WakeParams& params) {
    weScene = scene;
    params_ = params;
    generateFoamTexture();
}

void WickedKelvinWake::generateFoamTexture() {
    static const int SIZE = 256;
    static const int TILE = 32; // noise tiling period

    // Deterministic hash for tileable noise
    auto hash = [](int x, int y) -> float {
        int n = ((x * 1619) ^ (y * 31337)) & 0x7FFFFFFF;
        n = (n >> 13) ^ n;
        n = (n * (n * n * 60493 + 19990303) + 1376312589) & 0x7FFFFFFF;
        return (float)n / 2147483647.0f;
    };

    // Smooth tileable value noise
    auto smoothNoise = [&hash](float fx, float fy, int tile) -> float {
        int ix = (int)std::floor(fx);
        int iy = (int)std::floor(fy);
        float sx = fx - ix;
        float sy = fy - iy;
        sx = sx * sx * (3.0f - 2.0f * sx);
        sy = sy * sy * (3.0f - 2.0f * sy);
        int x0 = ((ix % tile) + tile) % tile;
        int y0 = ((iy % tile) + tile) % tile;
        int x1 = (x0 + 1) % tile;
        int y1 = (y0 + 1) % tile;
        float a = hash(x0, y0), b = hash(x1, y0);
        float c = hash(x0, y1), d = hash(x1, y1);
        return a + sx * (b - a) + sy * (c - a) + sx * sy * (a - b - c + d);
    };

    std::vector<uint8_t> pixels(SIZE * SIZE * 4);
    for (int y = 0; y < SIZE; y++) {
        for (int x = 0; x < SIZE; x++) {
            float fx = (float)x / SIZE * TILE;
            float fy = (float)y / SIZE * TILE;

            // Multi-octave noise for organic foam patches
            float n = 0;
            n += smoothNoise(fx, fy, TILE) * 0.50f;
            n += smoothNoise(fx * 2, fy * 2, TILE * 2) * 0.30f;
            n += smoothNoise(fx * 4, fy * 4, TILE * 4) * 0.20f;

            // Threshold into discrete foam patches
            float foam = std::max(0.0f, (n - 0.32f) / 0.35f);
            foam = std::min(1.0f, foam);

            uint8_t alpha = static_cast<uint8_t>(foam * 220.0f);
            int idx = (y * SIZE + x) * 4;
            pixels[idx + 0] = 255;
            pixels[idx + 1] = 252;
            pixels[idx + 2] = 248;
            pixels[idx + 3] = alpha;
        }
    }

    wi::graphics::TextureDesc desc;
    desc.width = SIZE;
    desc.height = SIZE;
    desc.format = wi::graphics::Format::R8G8B8A8_UNORM;
    desc.bind_flags = wi::graphics::BindFlag::SHADER_RESOURCE;
    desc.mip_levels = 1;
    desc.array_size = 1;

    wi::graphics::SubresourceData initData;
    initData.data_ptr = pixels.data();
    initData.row_pitch = SIZE * 4;
    initData.slice_pitch = initData.row_pitch * SIZE;

    wi::graphics::GetDevice()->CreateTexture(&desc, &initData, &foamTexture_);
}

WickedKelvinWake::ShipWake* WickedKelvinWake::findWake(int shipId) {
    for (auto& w : wakes) {
        if (w.shipId == shipId) return &w;
    }
    return nullptr;
}

WickedKelvinWake::ShipWake& WickedKelvinWake::getOrCreateWake(int shipId) {
    auto* existing = findWake(shipId);
    if (existing) return *existing;

    wakes.push_back({});
    auto& wake = wakes.back();
    wake.shipId = shipId;
    return wake;
}

void WickedKelvinWake::update(int shipId, const Vec3& position, float heading,
                                float speed, float dt) {
    if (!weScene || !visible_) return;

    auto& wake = getOrCreateWake(shipId);

    // Age existing trail points
    for (auto& p : wake.trail) {
        p.age += dt;
    }

    // Remove old trail points beyond max wake length
    float maxAge = params_.maxLength / std::max(speed, params_.speedThreshold);
    wake.trail.erase(
        std::remove_if(wake.trail.begin(), wake.trail.end(),
            [maxAge](const TrailPoint& p) { return p.age > maxAge; }),
        wake.trail.end()
    );

    // Add new trail point if ship is moving fast enough
    if (speed > params_.speedThreshold) {
        // Only add if moved far enough from last point (prevents clustering)
        float minSpacing = params_.maxLength / params_.numSegments;
        bool addPoint = wake.trail.empty();
        if (!addPoint) {
            const auto& last = wake.trail.front();
            float dx = position.x - last.position.x;
            float dz = position.z - last.position.z;
            addPoint = (dx * dx + dz * dz) > (minSpacing * minSpacing);
        }

        if (addPoint) {
            TrailPoint p;
            p.position = position;
            p.heading = heading;
            p.speed = speed;
            p.age = 0.0f;
            wake.trail.insert(wake.trail.begin(), p); // newest first

            // Cap trail length
            if ((int)wake.trail.size() > params_.numSegments) {
                wake.trail.resize(params_.numSegments);
            }

            wake.dirty = true;
        }
    }

    // Rebuild mesh if trail changed
    if (wake.dirty && wake.trail.size() >= 2) {
        rebuildWakeMesh(wake);
        wake.dirty = false;
        std::cout << "[Wake] ship=" << shipId << " trail=" << wake.trail.size()
                  << " spd=" << speed << " obj=" << wake.objectEntity << std::endl;
    }
}

void WickedKelvinWake::rebuildWakeMesh(ShipWake& wake) {
    if (!weScene) return;

    // Remove old entities
    removeWakeEntities(wake);

    if (wake.trail.size() < 2) return;

    // Create new mesh entity
    wake.objectEntity = weScene->Entity_CreateObject("BC_Wake_" + std::to_string(wake.shipId));
    wake.meshEntity = weScene->Entity_CreateMesh("BC_Wake_mesh_" + std::to_string(wake.shipId));
    weScene->Component_Attach(wake.meshEntity, wake.objectEntity);

    auto* object = weScene->objects.GetComponent(wake.objectEntity);
    auto* mesh = weScene->meshes.GetComponent(wake.meshEntity);
    if (!object || !mesh) return;

    object->meshID = wake.meshEntity;
    object->SetForeground(true); // Render in foreground pass (after ocean) to avoid depth-clip

    // Create semi-transparent wake material
    wake.materialEntity = weScene->Entity_CreateMaterial("BC_WakeMat_" + std::to_string(wake.shipId));
    weScene->Component_Attach(wake.materialEntity, wake.objectEntity);
    auto* material = weScene->materials.GetComponent(wake.materialEntity);
    if (material) {
        material->baseColor = DirectX::XMFLOAT4(0.9f, 0.92f, 0.95f, 1.0f);
        material->shaderType = MaterialComponent::SHADERTYPE_UNLIT;
        material->SetCastShadow(false);
        material->SetDoubleSided(true);
        material->userBlendMode = wi::enums::BLENDMODE_ALPHA;
        // Tile the foam texture: 2x across width, 15x along wake length
        material->texMulAdd = DirectX::XMFLOAT4(2.0f, 15.0f, 0.0f, 0.0f);
        if (foamTexture_.IsValid()) {
            material->textures[MaterialComponent::BASECOLORMAP].resource.SetTexture(foamTexture_);
        }
        material->CreateRenderData();
    }

    mesh->subsets.push_back(MeshComponent::MeshSubset());
    mesh->subsets.back().materialID = wake.materialEntity;
    mesh->subsets.back().indexOffset = 0;

    float kelvinAngle = KELVIN_HALF_ANGLE * DEG_TO_RAD;
    int numPoints = static_cast<int>(wake.trail.size());
    float maxAge = 0.0f;
    for (const auto& p : wake.trail) {
        if (p.age > maxAge) maxAge = p.age;
    }
    if (maxAge < 0.001f) maxAge = 1.0f;

    // Build wake geometry: 5 vertices per cross-section for realistic foam profile
    // Profile: edge(transparent) -> inner(subtle) -> center(bright foam) -> inner -> edge
    float foamPeak = params_.foamIntensity * 255.0f; // peak center alpha
    for (int i = 0; i < numPoints; i++) {
        const auto& pt = wake.trail[i];

        // Wake width expands with distance behind ship (Kelvin V-angle)
        float distBehind = pt.age * pt.speed;
        float halfWidth = distBehind * std::tan(kelvinAngle);
        halfWidth = std::min(halfWidth, params_.width * 0.5f);
        halfWidth = std::max(halfWidth, 1.0f); // minimum 1m half-width near stern

        // Fade out with age (cubic for faster tail fade)
        float fade = 1.0f - (pt.age / maxAge);
        fade = fade * fade * fade;

        // Cross direction (perpendicular to ship heading)
        float crossX = std::cos(pt.heading);
        float crossZ = -std::sin(pt.heading);

        float yBase = pt.position.y + 0.3f;
        DirectX::XMFLOAT3 up(0.0f, 1.0f, 0.0f);
        float u = static_cast<float>(i) / (numPoints - 1);

        // Alpha profile across the wake cross-section:
        //   edge=0, inner=low, center=peak foam, inner=low, edge=0
        uint8_t aEdge   = 0;
        uint8_t aInner  = static_cast<uint8_t>(fade * foamPeak * 0.25f);
        uint8_t aCenter = static_cast<uint8_t>(fade * foamPeak);

        uint32_t colEdge   = wi::Color(220, 225, 230, aEdge).rgba;
        uint32_t colInner  = wi::Color(230, 235, 240, aInner).rgba;
        uint32_t colCenter = wi::Color(245, 248, 252, aCenter).rgba;

        // 5 vertices: left edge, left inner, center, right inner, right edge
        float innerFrac = 0.3f; // inner verts at 30% of half-width from center

        DirectX::XMFLOAT3 positions[5] = {
            { pt.position.x - crossX * halfWidth,               yBase, pt.position.z - crossZ * halfWidth },
            { pt.position.x - crossX * halfWidth * innerFrac,   yBase, pt.position.z - crossZ * halfWidth * innerFrac },
            { pt.position.x,                                    yBase + 0.05f, pt.position.z },
            { pt.position.x + crossX * halfWidth * innerFrac,   yBase, pt.position.z + crossZ * halfWidth * innerFrac },
            { pt.position.x + crossX * halfWidth,               yBase, pt.position.z + crossZ * halfWidth },
        };
        uint32_t colors[5] = { colEdge, colInner, colCenter, colInner, colEdge };
        float uvX[5] = { 0.0f, 0.2f, 0.5f, 0.8f, 1.0f };

        for (int v = 0; v < 5; v++) {
            mesh->vertex_positions.push_back(positions[v]);
            mesh->vertex_normals.push_back(up);
            mesh->vertex_uvset_0.push_back(DirectX::XMFLOAT2(uvX[v], u));
            mesh->vertex_colors.push_back(colors[v]);
        }
    }

    // Generate triangle indices between cross-sections (5 verts per section = 4 quads)
    const int vertsPerSection = 5;
    for (int i = 0; i < numPoints - 1; i++) {
        uint32_t base = i * vertsPerSection;
        uint32_t next = (i + 1) * vertsPerSection;

        for (int q = 0; q < vertsPerSection - 1; q++) {
            mesh->indices.push_back(base + q);
            mesh->indices.push_back(next + q);
            mesh->indices.push_back(base + q + 1);

            mesh->indices.push_back(base + q + 1);
            mesh->indices.push_back(next + q);
            mesh->indices.push_back(next + q + 1);
        }
    }

    mesh->subsets.back().indexCount = static_cast<uint32_t>(mesh->indices.size());
    mesh->CreateRenderData();
}

void WickedKelvinWake::removeWakeEntities(ShipWake& wake) {
    if (!weScene) return;
    if (wake.objectEntity != INVALID_ENTITY) {
        weScene->Entity_Remove(wake.objectEntity);
        wake.objectEntity = INVALID_ENTITY;
        wake.meshEntity = INVALID_ENTITY;
        wake.materialEntity = INVALID_ENTITY;
    }
}

void WickedKelvinWake::removeWake(int shipId) {
    for (auto it = wakes.begin(); it != wakes.end(); ++it) {
        if (it->shipId == shipId) {
            removeWakeEntities(*it);
            wakes.erase(it);
            return;
        }
    }
}

void WickedKelvinWake::setVisible(bool visible) {
    visible_ = visible;
    if (!weScene) return;
    for (auto& wake : wakes) {
        if (wake.objectEntity != INVALID_ENTITY) {
            auto* obj = weScene->objects.GetComponent(wake.objectEntity);
            if (obj) obj->SetRenderable(visible);
        }
    }
}

void WickedKelvinWake::shutdown() {
    if (weScene) {
        for (auto& wake : wakes) {
            removeWakeEntities(wake);
        }
    }
    wakes.clear();
    weScene = nullptr;
}

}}} // namespace bc::graphics::wicked

#endif // WITH_WICKED_ENGINE
