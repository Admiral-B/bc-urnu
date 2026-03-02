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
    {  500.0f, 256,   300.0f, 0.4f,  200.0f, 5000.0f },  // Far swells
    {  250.0f, 512,   300.0f, 0.5f,    0.0f, 1000.0f },  // Primary wind waves (250m = invisible tiling)
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
    op.choppy_scale = p.choppyScale * 3.0f; // Compensate for GridLen reduction at patch=250m
    op.surfaceDisplacementTolerance = 2.0f + currentWeather_ * 0.5f;

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

        // Phillips spectrum (WE built-in) handles wind change automatically
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

static float fbmNoise(float x, float y, int octaves) {
    float value = 0.0f;
    float amplitude = 0.5f;
    float frequency = 1.0f;
    for (int i = 0; i < octaves; i++) {
        value += amplitude * smoothNoise2D(x * frequency, y * frequency);
        amplitude *= 0.5f;
        frequency *= 2.0f;
    }
    return value;
}

void WickedMultiCascadeOcean::generateNormalOverlay() {
    const int SIZE = 512;
    const float SCALE = 4.0f; // UV repetitions across the texture

    // Generate height field from multi-octave noise
    std::vector<float> heights(SIZE * SIZE);
    for (int y = 0; y < SIZE; y++) {
        for (int x = 0; x < SIZE; x++) {
            float u = (float)x / SIZE * SCALE;
            float v = (float)y / SIZE * SCALE;
            heights[y * SIZE + x] = fbmNoise(u + 73.1f, v + 149.7f, 5);
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

            // Normal = normalize(-dhdx, 1, -dhdy), but we only store XY
            // Scale gradient for visible but subtle effect
            float nx = -dhdx * 2.0f;
            float ny = -dhdy * 2.0f;
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
    // Choppy scale compensates for xOceanGridLen reduction at larger patch_length.
    // GridLen = dmap_dim/patch_length: at 250m it's 2.05 vs 10.24 at 50m (5x smaller).
    // 3x (not 5x) because longer-wavelength waves have smoother gradients.
    op.choppy_scale = configs[1].choppyScale * 3.0f;
    op.time_scale = 0.2f;
    op.waterHeight = waterHeight_;

    // Water appearance: dark murky green-grey (North Sea/Atlantic look).
    // Lower alpha = more light penetrates = less mirror-like reflection.
    op.waterColor = XMFLOAT4(0.01f, 0.03f, 0.025f, 0.25f);
    // Extinction: muted blue-green, not vivid blue. Controls subsurface color.
    op.extinctionColor = XMFLOAT4(0.12f, 0.35f, 0.28f, 1.0f);
    op.surfaceDetail = 4;
    op.surfaceDisplacementTolerance = 2.0f + currentWeather_ * 0.5f;

    // Set wind direction
    float dirX = std::sin(windDirRad);
    float dirZ = std::cos(windDirRad);
    op.wind_dir = XMFLOAT2(dirX, dirZ);
    // Wind speed capped to match OceanMath (prevents Phillips spectrum aliasing)
    op.wind_speed = std::max(30.0f, std::min(windSpeedMps * 100.0f, 2000.0f));
    op.wind_dependency = 0.07f; // Lower = more directional waves, less grid pattern

    // Phillips spectrum (WE built-in) is used. JONSWAP was tested but its peak
    // frequency at typical winds (B3-B7) corresponds to wavelengths > 50m,
    // placing most energy outside the FFT's representable range. Phillips'
    // broad k^-6 tail distributes energy well across 2-25m wavelengths.

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
    auxOp.time_scale = 0.3f;
    auxOp.waterHeight = waterHeight_;
    auxOp.wind_dir = XMFLOAT2(std::sin(windDirRad), std::cos(windDirRad));
    auxOp.wind_speed = std::max(30.0f, std::min(windSpeedMps * 100.0f, 1500.0f));
    auxOp.wind_dependency = 0.07f;

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

    op.choppy_scale = configs[1].choppyScale * 3.0f; // Compensate for GridLen at patch=250m
    op.surfaceDisplacementTolerance = 2.0f + currentWeather_ * 0.5f;

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

}}} // namespace bc::graphics::wicked

#endif // WITH_WICKED_ENGINE
