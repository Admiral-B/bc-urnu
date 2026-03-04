/*   Bridge Command 5.0 Ship Simulator
     Copyright (C) 2026 James Packer

     This program is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License version 2 as
     published by the Free Software Foundation */

#ifdef WITH_WICKED_ENGINE

#include "WickedMultiCascadeOcean.hpp"
#include "WickedOceanSpectrum.hpp"
#include "../../OceanMath.hpp"
#include "WickedEngine.h"
#include <cmath>
#include <iostream>
#include <vector>

using namespace DirectX;

// File-scope texture for normal overlay (persists for ocean lifetime)
static wi::graphics::Texture s_normalOverlayTexture;

namespace bc { namespace graphics { namespace wicked {

// Default cascade parameters:
// Cascade 0: Large swells (far distance, low resolution)
// Cascade 1: Wind waves (medium distance, high resolution) - primary cascade
// Cascade 2: Ripples/capillary (near distance, low resolution)
const WickedMultiCascadeOcean::CascadeConfig
WickedMultiCascadeOcean::DEFAULT_CONFIGS[NUM_CASCADES] = {
    // patchLength, fftRes, waveAmp, choppy, minDist, maxDist
    {  500.0f, 256,   300.0f, 0.4f,  200.0f, 5000.0f },  // Far swells (unused)
    { 1000.0f, 512,   300.0f, 0.5f,    0.0f, 1000.0f },  // Primary wind waves (1000m = invisible tiling from bridge)
    {   50.0f, 256,   300.0f, 0.6f,    0.0f,  200.0f },  // Near ripples (unused)
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

void WickedMultiCascadeOcean::update(float tideHeight, const Vec3& viewPosition,
                                      int /*lightLevel*/, float weather,
                                      float windSpeedKts, float windDirectionDeg) {
    if (!weScene) return;
    using namespace bc::OceanMath;

    currentWeather_ = weather;
    setWaterHeight(tideHeight);

    // Map Beaufort + wind to ocean parameters using shared math
    auto p = beaufortToOceanParams(weather, windSpeedKts, windDirectionDeg);

    // Fetch estimation: recompute when ship moves >500m or wind changes >15deg
    float windDirRad = std::atan2(p.windDirX, p.windDirZ);
    float shipX = viewPosition.x, shipZ = viewPosition.z;
    float moveDist = std::sqrt((shipX - cachedFetchX_) * (shipX - cachedFetchX_) +
                               (shipZ - cachedFetchZ_) * (shipZ - cachedFetchZ_));
    float windDirDelta = std::abs(windDirRad - cachedFetchWindDir_);
    if (windDirDelta > 3.14159f) windDirDelta = 6.28318f - windDirDelta;

    if (moveDist > 500.0f || windDirDelta > 0.26f || cachedFetchWindDir_ < -900.0f) {
        bool firstCompute = (cachedFetchWindDir_ < -900.0f);
        cachedFetch_ = estimateEffectiveFetch(shipX, shipZ, windDirRad, terrainHeightQuery_);
        cachedFetchX_ = shipX;
        cachedFetchZ_ = shipZ;
        cachedFetchWindDir_ = windDirRad;
        if (firstCompute) {
            float fs = fetchReductionFactor(p.windSpeedMps, cachedFetch_);
            std::cout << "[Ocean] Fetch estimate: " << (cachedFetch_ / 1000.0f)
                      << " km, wind=" << p.windSpeedMps << " m/s"
                      << ", reduction=" << fs
                      << ", amp " << p.waveAmplitude << " -> " << (p.waveAmplitude * fs)
                      << std::endl;
        }
    }

    // Apply fetch reduction to wave amplitude
    float fetchScale = fetchReductionFactor(p.windSpeedMps, cachedFetch_);
    float adjustedAmp = p.waveAmplitude * fetchScale;

    // Update primary cascade amplitude/choppiness from Beaufort
    auto& op = weScene->weather.oceanParameters;
    op.waterHeight = tideHeight;
    op.wave_amplitude = adjustedAmp;
    op.choppy_scale = p.choppyScale * 0.3f * std::sqrt(fetchScale);
    // WE uses CGS gravity (981 cm/s^2) in the dispersion relation omega=sqrt(g*|K|),
    // but K is in rad/m (from patch_length in meters). This makes omega 10x too fast.
    // time_scale = 0.1 exactly compensates: waves propagate at physically correct speed.
    op.time_scale = 0.1f;
    op.surfaceDisplacementTolerance = 2.0f + currentWeather_ * 0.5f;

    // Check if wind changed enough to regenerate spectrum
    float speedRatio = (lastWindSpeed_ > 0.5f)
        ? std::abs(p.windSpeedMps - lastWindSpeed_) / lastWindSpeed_
        : (p.windSpeedMps > 0.5f ? 1.0f : 0.0f);
    float dirDelta = std::abs(windDirRad - lastWindDir_);
    if (dirDelta > 3.14159f) dirDelta = 6.28318f - dirDelta;

    if (speedRatio > 0.3f || dirDelta > 0.52f) {
        op.wind_dir = XMFLOAT2(p.windDirX, p.windDirZ);
        op.wind_speed = p.windSpeedCmps;

        // Reset spectrumCallback with current wind params (captured by value)
        float wsCmps = op.wind_speed;
        float wAmp = op.wave_amplitude;
        op.spectrumCallback = [wsCmps, wAmp](float kx, float kz, float wdx, float wdz) -> float {
            return OceanSpectrum::PhillipsHasselmann(kx, kz, wdx, wdz, wsCmps, wAmp);
        };

        weScene->ocean.Create(op);
        lastWindSpeed_ = p.windSpeedMps;
        lastWindDir_ = windDirRad;
    }

    // Aux cascades disabled (same 50m tiling creates interference, not breakup)
    op.cascade0Weight = 0.0f;
    op.cascade2Weight = 0.0f;
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

// Hash-based noise for normal map generation (no external dependency)
static float hash2D(float x, float y) {
    // Fast deterministic hash
    int ix = (int)(x * 127.1f + y * 311.7f);
    ix = (ix << 13) ^ ix;
    return 1.0f - ((ix * (ix * ix * 15731 + 789221) + 1376312589) & 0x7fffffff) / 1073741824.0f;
}

static float smoothNoise2D(float x, float y) {
    float ix = std::floor(x);
    float iy = std::floor(y);
    float fx = x - ix;
    float fy = y - iy;
    // Smoothstep interpolation
    float ux = fx * fx * (3.0f - 2.0f * fx);
    float uy = fy * fy * (3.0f - 2.0f * fy);
    float a = hash2D(ix, iy);
    float b = hash2D(ix + 1.0f, iy);
    float c = hash2D(ix, iy + 1.0f);
    float d = hash2D(ix + 1.0f, iy + 1.0f);
    return a + (b - a) * ux + (c - a) * uy + (a - b - c + d) * ux * uy;
}

// Ridged multifractal noise: creates sharp crests like real capillary waves.
// Unlike smooth FBM, abs(noise) produces sharp zero-crossings that look like wave ridges.
static float ridgedNoise(float x, float y, int octaves, float lacunarity = 2.0f) {
    float value = 0.0f;
    float amplitude = 1.0f;
    float frequency = 1.0f;
    float weight = 1.0f;
    for (int i = 0; i < octaves; i++) {
        float signal = 1.0f - std::abs(smoothNoise2D(x * frequency, y * frequency));
        signal *= signal; // Square for sharper crests
        signal *= weight;
        weight = std::min(1.0f, std::max(0.0f, signal * 2.0f)); // Crests seed next octave
        value += signal * amplitude;
        amplitude *= 0.5f;
        frequency *= lacunarity;
    }
    return value;
}

void WickedMultiCascadeOcean::generateNormalOverlay() {
    const int SIZE = 512;
    const float SCALE = 6.0f; // UV repetitions across the texture

    // Generate height field using ridged noise for sharp wave-crest features.
    // Two layers at different orientations break the single-direction look.
    std::vector<float> heights(SIZE * SIZE);
    for (int y = 0; y < SIZE; y++) {
        for (int x = 0; x < SIZE; x++) {
            float u = (float)x / SIZE * SCALE;
            float v = (float)y / SIZE * SCALE;

            // Primary wave crests: slightly stretched along one axis (anisotropic)
            float h1 = ridgedNoise(u * 1.0f + 73.1f, v * 1.6f + 149.7f, 6);
            // Secondary crests: rotated ~60 degrees, different offset
            float u2 = u * 0.866f - v * 0.5f;
            float v2 = u * 0.5f + v * 0.866f;
            float h2 = ridgedNoise(u2 * 1.3f + 37.9f, v2 * 1.0f + 213.4f, 5);

            heights[y * SIZE + x] = h1 * 0.6f + h2 * 0.4f;
        }
    }

    // Convert height field to normal map (Sobel-like finite differences)
    // Store as RGBA8: RG = normal XY [-1,1] mapped to [0,255], BA = unused
    std::vector<uint8_t> pixels(SIZE * SIZE * 4);
    for (int y = 0; y < SIZE; y++) {
        for (int x = 0; x < SIZE; x++) {
            int xp = (x + 1) % SIZE;
            int xm = (x + SIZE - 1) % SIZE;
            int yp = (y + 1) % SIZE;
            int ym = (y + SIZE - 1) % SIZE;

            float dhdx = heights[y * SIZE + xp] - heights[y * SIZE + xm];
            float dhdy = heights[yp * SIZE + x] - heights[ym * SIZE + x];

            // Normal = normalize(-dhdx, 1, -dhdy), store XY in [0,1] encoding.
            // Scale 3x for pronounced ridged features (sampled with amplitude control in PS).
            float nx = -dhdx * 3.0f;
            float ny = -dhdy * 3.0f;
            // Clamp to [-1, 1]
            nx = std::max(-1.0f, std::min(1.0f, nx));
            ny = std::max(-1.0f, std::min(1.0f, ny));

            int idx = (y * SIZE + x) * 4;
            pixels[idx + 0] = (uint8_t)((nx * 0.5f + 0.5f) * 255.0f); // R = normal X
            pixels[idx + 1] = (uint8_t)((ny * 0.5f + 0.5f) * 255.0f); // G = normal Y
            pixels[idx + 2] = 128; // B = normal Z (up, ~0.5 encoded)
            pixels[idx + 3] = 255; // A = 1
        }
    }

    // Create GPU texture
    wi::graphics::TextureDesc desc;
    desc.width = SIZE;
    desc.height = SIZE;
    desc.format = wi::graphics::Format::R8G8B8A8_UNORM;
    desc.bind_flags = wi::graphics::BindFlag::SHADER_RESOURCE;
    desc.usage = wi::graphics::Usage::DEFAULT;
    desc.mip_levels = 1;
    desc.array_size = 1;
    desc.sample_count = 1;
    desc.layout = wi::graphics::ResourceState::SHADER_RESOURCE;

    wi::graphics::SubresourceData initData;
    initData.data_ptr = pixels.data();
    initData.row_pitch = SIZE * 4;

    auto* device = wi::graphics::GetDevice();
    device->CreateTexture(&desc, &initData, &s_normalOverlayTexture);
    device->SetName(&s_normalOverlayTexture, "ocean_normal_overlay");

    normalOverlayDescIdx_ = device->GetDescriptorIndex(&s_normalOverlayTexture,
                                                        wi::graphics::SubresourceType::SRV);

    std::cout << "WickedMultiCascadeOcean: Normal overlay texture created ("
              << SIZE << "x" << SIZE << ", desc=" << normalOverlayDescIdx_ << ")" << std::endl;
}

void WickedMultiCascadeOcean::init(wi::scene::Scene* scene,
                                     float windSpeedMps, float windDirRad) {
    weScene = scene;
    if (!weScene) return;

    // Enable ocean in the weather component
    weScene->weather.SetOceanEnabled(true);

    // Generate normal overlay texture for large-scale tiling breakup
    if (normalOverlayDescIdx_ < 0) {
        generateNormalOverlay();
    }

    // The primary cascade (index 1) drives WE's built-in scene ocean.
    // This ensures the standard rendering pipeline works out of the box.
    // Cascades 0 and 2 are auxiliary and will provide additional textures
    // for the multi-cascade surface shader (Phase 3 shader work).

    // Configure the scene's primary ocean (cascade 1 = medium/wind waves)
    auto& op = weScene->weather.oceanParameters;
    op.patch_length = configs[1].patchLength;
    op.dmap_dim = configs[1].fftResolution;
    op.wave_amplitude = configs[1].waveAmplitude;
    op.choppy_scale = configs[1].choppyScale * 0.3f;
    // WE uses CGS gravity (981 cm/s^2) in the dispersion relation omega=sqrt(g*|K|),
    // but K is in rad/m (from patch_length in meters). This makes omega 10x too fast.
    // time_scale = 0.1 exactly compensates: waves propagate at physically correct speed.
    op.time_scale = 0.1f;
    op.waterHeight = waterHeight_;

    // Water appearance: grey-green (North Sea/Atlantic).
    // Higher albedo so body color is visible, not pure black at grazing angles.
    op.waterColor = XMFLOAT4(0.03f, 0.07f, 0.06f, 0.3f);
    // Extinction: blue-green subsurface tint. Controls light absorption with depth.
    op.extinctionColor = XMFLOAT4(0.15f, 0.40f, 0.32f, 1.0f);
    // surfaceDetail=3 -> 480x270 screen-space grid, smoother horizon wave shapes
    op.surfaceDetail = 3;
    op.surfaceDisplacementTolerance = 2.0f + currentWeather_ * 0.5f;

    // Set wind direction
    float dirX = std::sin(windDirRad);
    float dirZ = std::cos(windDirRad);
    op.wind_dir = XMFLOAT2(dirX, dirZ);
    // Wind speed capped to match OceanMath (prevents Phillips spectrum aliasing)
    op.wind_speed = std::max(30.0f, std::min(windSpeedMps * 100.0f, 2000.0f));
    op.wind_dependency = 0.4f; // Fallback; overridden by spectrumCallback below

    // Phillips spectrum with Hasselmann frequency-dependent directional spreading.
    // Replaces WE's cos^2(theta) spreading which creates uniform parallel ridges.
    // Hasselmann: long waves are directional (realistic), short waves spread broadly
    // (creating the chaotic multi-directional surface of a real ocean).
    float wsCmps = op.wind_speed;
    float wAmp = op.wave_amplitude;
    op.spectrumCallback = [wsCmps, wAmp](float kx, float kz, float wdx, float wdz) -> float {
        return OceanSpectrum::PhillipsHasselmann(kx, kz, wdx, wdz, wsCmps, wAmp);
    };

    // Bind normal overlay texture for tiling breakup
    op.normalOverlayTextureIndex = normalOverlayDescIdx_;

    // Create the primary ocean
    weScene->ocean.Create(op);
    cascadeData[1].initialized = true;

    lastWindSpeed_ = windSpeedMps;
    lastWindDir_ = windDirRad;

    // Auxiliary cascades (0 and 2) are disabled for now. With patch_length=250m
    // on the primary cascade, geometric tiling is invisible at ship scale.
    // Future: aux cascades at different patch sizes (500m, 50m) for multi-scale detail.

    // Diagnostic: compare Hasselmann vs cos^2 for a few wave modes
    {
        float testK = 2.0f * 3.14159f / configs[1].patchLength; // lowest non-zero k
        float wdx = op.wind_dir.x, wdz = op.wind_dir.y;
        // Downwind mode (kx=testK, kz=0 with wind along x)
        float downwind = OceanSpectrum::PhillipsHasselmann(testK * wdx, testK * wdz, wdx, wdz, wsCmps, wAmp);
        // Cross-wind mode (perpendicular)
        float crosswind = OceanSpectrum::PhillipsHasselmann(-testK * wdz, testK * wdx, wdx, wdz, wsCmps, wAmp);
        // Phillips reference: cos^2=1 downwind, cos^2=0 crosswind
        float phillipsRef = OceanSpectrum::Phillips(testK * testK, testK, wsCmps, wAmp * 1e-7f, 0.4f);
        std::cout << "  Spectrum check at k=" << testK << ": downwind=" << downwind
                  << " crosswind=" << crosswind << " ratio=" << (downwind > 0 ? crosswind / downwind : 0)
                  << " (Phillips ref sqrt=" << std::sqrt(phillipsRef) << ")" << std::endl;
    }

    std::cout << "WickedMultiCascadeOcean: Primary cascade initialized (patch="
              << configs[1].patchLength << "m, fft=" << configs[1].fftResolution
              << ", amp=" << configs[1].waveAmplitude << ")" << std::endl;
}

void WickedMultiCascadeOcean::createCascade(int index, float windSpeedMps, float windDirRad) {
    if (index == 1) return; // Primary uses scene ocean

    auto& cd = cascadeData[index];
    cd.ocean = std::make_unique<wi::Ocean>();

    wi::Ocean::OceanParameters auxOp;
    auxOp.patch_length = configs[index].patchLength;
    auxOp.dmap_dim = configs[index].fftResolution;
    auxOp.wave_amplitude = configs[index].waveAmplitude;
    auxOp.choppy_scale = configs[index].choppyScale;
    auxOp.time_scale = 0.1f;
    auxOp.waterHeight = waterHeight_;
    auxOp.wind_dir = XMFLOAT2(std::sin(windDirRad), std::cos(windDirRad));
    auxOp.wind_speed = std::max(30.0f, std::min(windSpeedMps * 100.0f, 1500.0f));
    auxOp.wind_dependency = 0.4f;

    // Different random seed per cascade for phase diversity
    auxOp.randomSeed = 42 + index * 1337;

    // JONSWAP spectrum for aux cascades too
    float fetchKm = bc::OceanMath::beaufortToFetchKm(currentWeather_);
    float gamma = bc::OceanMath::beaufortToGamma(currentWeather_);
    float ws = windSpeedMps;
    float pl = auxOp.patch_length;
    auxOp.spectrumCallback = [ws, fetchKm, gamma, pl](float kx, float kz, float wdx, float wdz) -> float {
        return OceanSpectrum::JONSWAPAmplitude(kx, kz, ws, wdx, wdz, pl, fetchKm, gamma);
    };

    cd.ocean->Create(auxOp);
    cd.initialized = true;

    std::cout << "  Cascade " << index << " created: seed=" << auxOp.randomSeed
              << " fft=" << auxOp.dmap_dim << " amp=" << auxOp.wave_amplitude << std::endl;
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
    op.wind_speed = std::max(30.0f, std::min(windSpeedMps * 100.0f, 1500.0f));

    op.choppy_scale = configs[1].choppyScale * 0.3f;
    op.surfaceDisplacementTolerance = 2.0f + currentWeather_ * 0.5f;

    // Phillips + Hasselmann spreading (must reset callback with new wind params)
    float wsCmps = op.wind_speed;
    float wAmp = op.wave_amplitude;
    op.spectrumCallback = [wsCmps, wAmp](float kx, float kz, float wdx, float wdz) -> float {
        return OceanSpectrum::PhillipsHasselmann(kx, kz, wdx, wdz, wsCmps, wAmp);
    };

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

    if (cascadeIndex == 1 && weScene) {
        return weScene->ocean.getDisplacementMap();
    }

    auto& cd = cascadeData[cascadeIndex];
    if (cd.ocean && cd.ocean->IsValid()) {
        return cd.ocean->getDisplacementMap();
    }
    return nullptr;
}

const void* WickedMultiCascadeOcean::getGradientMap(int cascadeIndex) const {
    if (cascadeIndex < 0 || cascadeIndex >= NUM_CASCADES) return nullptr;

    if (cascadeIndex == 1 && weScene) {
        return weScene->ocean.getGradientMap();
    }

    auto& cd = cascadeData[cascadeIndex];
    if (cd.ocean && cd.ocean->IsValid()) {
        return cd.ocean->getGradientMap();
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
        cascadeData[i].ocean.reset();
    }
    normalOverlayDescIdx_ = -1;
    weScene = nullptr;
}

void WickedMultiCascadeOcean::setTerrainHeightQuery(std::function<float(float, float)> query) {
    terrainHeightQuery_ = std::move(query);
}

}}} // namespace bc::graphics::wicked

#endif // WITH_WICKED_ENGINE
