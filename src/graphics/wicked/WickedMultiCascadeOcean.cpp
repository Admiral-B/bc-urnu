/*   Bridge Command 5.0 Ship Simulator
     Copyright (C) 2026 James Packer

     This program is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License version 2 as
     published by the Free Software Foundation */

#ifdef WITH_WICKED_ENGINE

#include "WickedMultiCascadeOcean.hpp"
#include "../../OceanMath.hpp"
#include "WickedEngine.h"
#include <cmath>
#include <iostream>

using namespace DirectX;

namespace bc { namespace graphics { namespace wicked {

// Default cascade parameters:
// Cascade 0: Large swells (far distance, low resolution)
// Cascade 1: Wind waves (medium distance, high resolution) - primary cascade
// Cascade 2: Ripples/capillary (near distance, low resolution)
const WickedMultiCascadeOcean::CascadeConfig
WickedMultiCascadeOcean::DEFAULT_CONFIGS[NUM_CASCADES] = {
    // patchLength, fftRes, waveAmp, choppy, minDist, maxDist
    {   50.0f, 256,   300.0f, 0.4f,  200.0f, 5000.0f },  // Far swells
    {   50.0f, 512,   300.0f, 0.5f,    0.0f, 1000.0f },  // Primary wind waves
    {   50.0f, 256,   300.0f, 0.6f,    0.0f,  200.0f },  // Near ripples
};

WickedMultiCascadeOcean::WickedMultiCascadeOcean() {
    for (int i = 0; i < NUM_CASCADES; i++) {
        configs[i] = DEFAULT_CONFIGS[i];
    }
}

WickedMultiCascadeOcean::~WickedMultiCascadeOcean() {
    shutdown();
}

void WickedMultiCascadeOcean::load(wi::scene::Scene* scene, float weather, int /*segments*/) {
    using namespace bc::OceanMath;
    currentWeather_ = weather;

    // Convert Beaufort to wind parameters for init
    auto p = beaufortToOceanParams(weather, 0.0f, 0.0f);

    // Set amplitudes on cascade configs before init
    configs[0].waveAmplitude = p.waveAmplitude * 0.3f;  // swells: 30% of primary
    configs[1].waveAmplitude = p.waveAmplitude;          // primary wind waves
    configs[2].waveAmplitude = p.waveAmplitude * 0.15f;  // ripples: 15% of primary

    configs[0].choppyScale = p.choppyScale * 0.6f;
    configs[1].choppyScale = p.choppyScale;
    configs[2].choppyScale = p.choppyScale * 1.2f;

    float windDirRad = std::atan2(p.windDirX, p.windDirZ);
    init(scene, p.windSpeedMps, windDirRad);
}

void WickedMultiCascadeOcean::update(float tideHeight, const Vec3& /*viewPosition*/,
                                      int /*lightLevel*/, float weather,
                                      float windSpeedKts, float windDirectionDeg) {
    if (!weScene) return;
    using namespace bc::OceanMath;

    currentWeather_ = weather;
    setWaterHeight(tideHeight);

    // Map Beaufort + wind to ocean parameters using shared math
    auto p = beaufortToOceanParams(weather, windSpeedKts, windDirectionDeg);

    // Update primary cascade amplitude/choppiness from Beaufort
    auto& op = weScene->weather.oceanParameters;
    op.waterHeight = tideHeight;
    op.wave_amplitude = p.waveAmplitude;
    op.choppy_scale = p.choppyScale;
    op.surfaceDisplacementTolerance = 2.0f;

    // Check if wind changed enough to regenerate spectrum
    float windDirRad = std::atan2(p.windDirX, p.windDirZ);
    float speedRatio = (lastWindSpeed_ > 0.5f)
        ? std::abs(p.windSpeedMps - lastWindSpeed_) / lastWindSpeed_
        : (p.windSpeedMps > 0.5f ? 1.0f : 0.0f);
    float dirDelta = std::abs(windDirRad - lastWindDir_);
    if (dirDelta > 3.14159f) dirDelta = 6.28318f - dirDelta;

    if (speedRatio > 0.3f || dirDelta > 0.52f) {
        op.wind_dir = XMFLOAT2(p.windDirX, p.windDirZ);
        op.wind_speed = p.windSpeedCmps;
        weScene->ocean.Create(op);
        lastWindSpeed_ = p.windSpeedMps;
        lastWindDir_ = windDirRad;
    }
}

Vec3 WickedMultiCascadeOcean::getPosition() const {
    return Vec3(0.0f, waterHeight_, 0.0f);
}

void WickedMultiCascadeOcean::setVisible(bool visible) {
    visible_ = visible;
    if (weScene) {
        weScene->weather.SetOceanEnabled(visible);
    }
}

void WickedMultiCascadeOcean::setCascadeConfig(int index, const CascadeConfig& config) {
    if (index < 0 || index >= NUM_CASCADES) return;
    configs[index] = config;
}

void WickedMultiCascadeOcean::init(wi::scene::Scene* scene,
                                     float windSpeedMps, float windDirRad) {
    weScene = scene;
    if (!weScene) return;

    // Enable ocean in the weather component
    weScene->weather.SetOceanEnabled(true);

    // The primary cascade (index 1) drives WE's built-in scene ocean.
    // This ensures the standard rendering pipeline works out of the box.
    // Cascades 0 and 2 are auxiliary and will provide additional textures
    // for the multi-cascade surface shader (Phase 3 shader work).

    // Configure the scene's primary ocean (cascade 1 = medium/wind waves)
    auto& op = weScene->weather.oceanParameters;
    op.patch_length = configs[1].patchLength;
    op.dmap_dim = configs[1].fftResolution;
    op.wave_amplitude = configs[1].waveAmplitude;
    op.choppy_scale = std::min(configs[1].choppyScale, 1.5f); // cap to prevent universal foam
    op.time_scale = 0.3f; // Wave animation speed (0.3 = natural period for 50m patch)
    op.waterHeight = waterHeight_;
    op.waterColor = XMFLOAT4(0.02f, 0.05f, 0.04f, 0.5f);
    op.extinctionColor = XMFLOAT4(0.05f, 0.6f, 0.85f, 1.0f);
    op.surfaceDetail = 4;
    op.surfaceDisplacementTolerance = 2.0f;

    // Set wind direction
    float dirX = std::sin(windDirRad);
    float dirZ = std::cos(windDirRad);
    op.wind_dir = XMFLOAT2(dirX, dirZ);
    op.wind_speed = std::max(30.0f, windSpeedMps * 100.0f);
    op.wind_dependency = 0.35f; // 0.35 spreads energy to opposing wind directions

    // Create the primary ocean
    weScene->ocean.Create(op);
    cascadeData[1].initialized = true;

    lastWindSpeed_ = windSpeedMps;
    lastWindDir_ = windDirRad;

    // Note: Auxiliary cascades (0 and 2) are created as separate wi::Ocean
    // instances. They run their own FFT but share the scene's rendering.
    // For now, the framework creates the primary cascade and provides
    // the infrastructure for adding cascades 0 and 2 when the custom
    // multi-cascade shaders are integrated.

    // Create auxiliary cascades
    createCascade(0, windSpeedMps, windDirRad);
    createCascade(2, windSpeedMps, windDirRad);

    std::cout << "WickedMultiCascadeOcean: Initialized " << NUM_CASCADES << " cascades:"
              << std::endl;
    for (int i = 0; i < NUM_CASCADES; i++) {
        std::cout << "  Cascade " << i << ": patch=" << configs[i].patchLength
                  << "m, fft=" << configs[i].fftResolution
                  << ", amp=" << configs[i].waveAmplitude
                  << ", range=[" << configs[i].minDistance
                  << "-" << configs[i].maxDistance << "m]"
                  << std::endl;
    }
}

void WickedMultiCascadeOcean::createCascade(int index, float windSpeedMps, float windDirRad) {
    // Auxiliary cascades are managed as separate wi::Ocean objects.
    // Their displacement/gradient textures will be bound as extra textures
    // in the custom multi-cascade surface shader.
    //
    // For now we mark them as initialized to track state.
    // The actual wi::Ocean instances for cascades 0 and 2 will be created
    // when we add the custom shader pipeline that can consume their outputs.
    //
    // The primary cascade (1) uses the scene's built-in ocean,
    // so the standard rendering pipeline works immediately.
    cascadeData[index].initialized = true;
}

void WickedMultiCascadeOcean::updateWind(float windSpeedMps, float windDirRad) {
    if (!weScene) return;

    // Only recreate spectra if wind changed significantly
    float speedDelta = std::abs(windSpeedMps - lastWindSpeed_);
    float dirDelta = std::abs(windDirRad - lastWindDir_);

    if (speedDelta < 0.5f && dirDelta < 0.05f) return;

    lastWindSpeed_ = windSpeedMps;
    lastWindDir_ = windDirRad;

    // Update primary cascade via scene weather
    auto& op = weScene->weather.oceanParameters;
    float dirX = std::sin(windDirRad);
    float dirZ = std::cos(windDirRad);
    op.wind_dir = XMFLOAT2(dirX, dirZ);
    op.wind_speed = std::max(30.0f, windSpeedMps * 100.0f);

    op.choppy_scale = configs[1].choppyScale;
    op.surfaceDisplacementTolerance = 2.0f;

    // Recreate the primary ocean with new spectrum
    weScene->ocean.Create(op);
}

void WickedMultiCascadeOcean::setWaterHeight(float height) {
    waterHeight_ = height;
    if (weScene) {
        weScene->weather.oceanParameters.waterHeight = height;
    }
}

float WickedMultiCascadeOcean::getWaveHeight(float worldX, float worldZ) const {
    if (!weScene || !weScene->weather.IsOceanEnabled()) return waterHeight_;

    // Use WE's built-in readback for the primary cascade
    // When auxiliary cascades are fully implemented, their displacements
    // would be added here for a blended result
    XMFLOAT3 queryPos(worldX, 0.0f, worldZ);
    XMFLOAT3 displaced = weScene->GetOceanPosAt(queryPos);
    return displaced.y;
}

Vec2 WickedMultiCascadeOcean::getLocalNormals(float worldX, float worldZ) const {
    if (!weScene || !weScene->weather.IsOceanEnabled()) return Vec2(0.0f, 0.0f);

    float hCenter = getWaveHeight(worldX, worldZ);
    float hRight = getWaveHeight(worldX + NORMAL_SAMPLE_OFFSET, worldZ);
    float hForward = getWaveHeight(worldX, worldZ + NORMAL_SAMPLE_OFFSET);

    float slopeX = (hRight - hCenter) / NORMAL_SAMPLE_OFFSET;
    float slopeZ = (hForward - hCenter) / NORMAL_SAMPLE_OFFSET;

    return Vec2(slopeX, slopeZ);
}

const void* WickedMultiCascadeOcean::getDisplacementMap(int cascadeIndex) const {
    if (cascadeIndex < 0 || cascadeIndex >= NUM_CASCADES) return nullptr;

    // Primary cascade uses the scene ocean's displacement map
    if (cascadeIndex == 1 && weScene) {
        return weScene->ocean.getDisplacementMap();
    }

    // Auxiliary cascades would return their own displacement maps
    // when fully implemented with separate wi::Ocean instances
    return nullptr;
}

const void* WickedMultiCascadeOcean::getGradientMap(int cascadeIndex) const {
    if (cascadeIndex < 0 || cascadeIndex >= NUM_CASCADES) return nullptr;

    if (cascadeIndex == 1 && weScene) {
        return weScene->ocean.getGradientMap();
    }

    return nullptr;
}

const WickedMultiCascadeOcean::CascadeConfig&
WickedMultiCascadeOcean::getCascadeConfig(int index) const {
    static const CascadeConfig empty = {};
    if (index < 0 || index >= NUM_CASCADES) return empty;
    return configs[index];
}

bool WickedMultiCascadeOcean::isValid() const {
    return weScene && weScene->ocean.IsValid() && cascadeData[1].initialized;
}

void WickedMultiCascadeOcean::shutdown() {
    for (int i = 0; i < NUM_CASCADES; i++) {
        cascadeData[i].initialized = false;
    }
    weScene = nullptr;
}

}}} // namespace bc::graphics::wicked

#endif // WITH_WICKED_ENGINE
