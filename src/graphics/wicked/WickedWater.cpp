/*   Bridge Command 5.0 Ship Simulator
     Copyright (C) 2026 James Packer

     This program is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License version 2 as
     published by the Free Software Foundation */

#ifdef WITH_WICKED_ENGINE

#include "WickedWater.hpp"
#include "WickedEngine.h"
#include <cmath>
#include <iostream>

using namespace DirectX;

namespace bc { namespace graphics { namespace wicked {

WickedWater::WickedWater() = default;
WickedWater::~WickedWater() = default;

void WickedWater::load(wi::scene::Scene* scene, float weather, int /*segments*/) {
    weScene = scene;
    currentWeather_ = weather;

    if (!weScene) return;

    weScene->weather.SetOceanEnabled(true);

    auto& op = weScene->weather.oceanParameters;

    op.dmap_dim = 512;
    // patch_length=50 is the only value that produces correct mesh geometry.
    // WE's adaptive ocean mesh has 2^surfaceDetail subdivisions per patch;
    // larger patches cause extreme vertex spacing and mesh breakdown.
    op.patch_length = 50.0f;
    op.time_scale = 0.3f;          // WE default; looks natural
    op.surfaceDetail = 4;
    op.surfaceDisplacementTolerance = 2.0f;

    op.waterColor = XMFLOAT4(0.02f, 0.05f, 0.04f, 0.5f);
    op.extinctionColor = XMFLOAT4(0.05f, 0.6f, 0.85f, 1.0f);

    op.waterHeight = 0.0f;

    // Beaufort-to-wind mapping (midpoint of each Beaufort range in knots)
    static const float beaufortToKnots[] = {0,2,5,8.5f,13,19,24,30,37,44,52,60,68};
    int bi = std::max(0, std::min(12, (int)weather));
    float frac = weather - bi;
    float nextKts = (bi < 12) ? beaufortToKnots[bi + 1] : beaufortToKnots[12];
    float approxWindKts = beaufortToKnots[bi] + frac * (nextKts - beaufortToKnots[bi]);
    float windMps = approxWindKts * 0.5144f;

    // Beaufort-scaled wave_amplitude. Keep low for calm scenarios --
    // visual detail comes from the 512x512 gradient/normal map in the shader,
    // not from mesh displacement. From a ferry bridge at 30m, even B5 (2m Hs)
    // looks almost flat; only geometry displacement at high Beaufort matters.
    static const float beaufortAmplitude[] = {
    //  B0 B1  B2  B3   B4   B5   B6   B7    B8    B9   B10   B11   B12
        2,  8, 20, 40, 100, 200, 350, 500,  800, 1200, 1800, 2500, 3500
    };
    float nextAmp = (bi < 12) ? beaufortAmplitude[bi + 1] : beaufortAmplitude[12];
    op.wave_amplitude = beaufortAmplitude[bi] + frac * (nextAmp - beaufortAmplitude[bi]);

    // choppy_scale drives Jacobian fold -> foam. WE default is 1.3.
    // Safe at low Beaufort because amplitude is small (no mesh fold-over).
    op.choppy_scale = 0.8f + weather * 0.1f;

    // WE wind_speed is in cm/s (gravity = 981 cm/s^2 in Phillips spectrum)
    op.wind_speed = std::max(30.0f, windMps * 100.0f);
    op.wind_dir = XMFLOAT2(0.0f, 1.0f); // default north, updated by update()
    // 0.35 spreads energy to opposing wind directions, breaking up regularity.
    op.wind_dependency = 0.35f;

    lastWindSpeedMps_ = windMps;
    lastWindDirRad_ = 0.0f;

    weScene->ocean.Create(op);

    std::cout << "WickedWater: Initialized (patch=" << op.patch_length
              << "m, amp=" << op.wave_amplitude
              << ", wind=" << op.wind_speed << "cm/s)" << std::endl;
}

void WickedWater::update(float tideHeight, const Vec3& /*viewPosition*/,
                          int /*lightLevel*/, float weather,
                          float windSpeedKts, float windDirectionDeg) {
    if (!weScene) return;

    tideHeight_ = tideHeight;
    currentWeather_ = weather;

    auto& op = weScene->weather.oceanParameters;

    op.waterHeight = tideHeight;

    float windMps = windSpeedKts * 0.5144f;

    // Wind direction: BC meteorological convention (FROM) -> wave propagation (WITH)
    float windRad = (windDirectionDeg + 180.0f) * 3.14159265f / 180.0f;
    float dirX = std::sin(windRad);
    float dirZ = std::cos(windRad);
    if (windMps < 0.3f) { dirX = 0.0f; dirZ = 1.0f; }
    op.wind_dir = XMFLOAT2(dirX, dirZ);

    // WE wind_speed is in cm/s
    op.wind_speed = std::max(30.0f, windMps * 100.0f);

    // Beaufort-scaled wave_amplitude and choppy_scale (must match load() table)
    static const float beaufortAmplitude[] = {
    //  B0 B1  B2  B3   B4   B5   B6   B7    B8    B9   B10   B11   B12
        2,  8, 20, 40, 100, 200, 350, 500,  800, 1200, 1800, 2500, 3500
    };
    int bi = std::max(0, std::min(12, (int)weather));
    float frac = weather - bi;
    float nextAmp = (bi < 12) ? beaufortAmplitude[bi + 1] : beaufortAmplitude[12];
    op.wave_amplitude = beaufortAmplitude[bi] + frac * (nextAmp - beaufortAmplitude[bi]);
    op.choppy_scale = 0.8f + weather * 0.1f;
    op.surfaceDisplacementTolerance = 2.0f;

    // Only recreate spectrum when wind changes significantly.
    // Create() regenerates H(0) with new random numbers, causing a visual pop.
    float speedRatio = (lastWindSpeedMps_ > 0.5f)
        ? std::abs(windMps - lastWindSpeedMps_) / lastWindSpeedMps_
        : (windMps > 0.5f ? 1.0f : 0.0f);
    float dirDelta = std::abs(windRad - lastWindDirRad_);
    if (dirDelta > 3.14159f) dirDelta = 6.28318f - dirDelta; // wrap

    if (speedRatio > 0.3f || dirDelta > 0.52f) { // >30% speed or >30 degrees
        weScene->ocean.Create(op);
        lastWindSpeedMps_ = windMps;
        lastWindDirRad_ = windRad;
    }
}

float WickedWater::getWaveHeight(float worldX, float worldZ) const {
    if (!weScene || !weScene->weather.IsOceanEnabled()) return tideHeight_;

    // Use WE's built-in CPU readback of displacement map
    XMFLOAT3 queryPos(worldX, 0.0f, worldZ);
    XMFLOAT3 displaced = weScene->GetOceanPosAt(queryPos);
    return displaced.y;
}

Vec2 WickedWater::getLocalNormals(float worldX, float worldZ) const {
    if (!weScene || !weScene->weather.IsOceanEnabled()) return Vec2(0.0f, 0.0f);

    // Approximate surface normal via finite differences of displaced height
    float hCenter = getWaveHeight(worldX, worldZ);
    float hRight = getWaveHeight(worldX + NORMAL_SAMPLE_OFFSET, worldZ);
    float hForward = getWaveHeight(worldX, worldZ + NORMAL_SAMPLE_OFFSET);

    // Normal = normalize(cross(tangentX, tangentZ))
    // tangentX = (NORMAL_SAMPLE_OFFSET, hRight - hCenter, 0)
    // tangentZ = (0, hForward - hCenter, NORMAL_SAMPLE_OFFSET)
    // cross = (-(hRight-hCenter)*offset, offset*offset, -(hForward-hCenter)*offset) -- not needed fully
    // Simplified: slope_x = (hRight - hCenter) / offset, slope_z = (hForward - hCenter) / offset
    float slopeX = (hRight - hCenter) / NORMAL_SAMPLE_OFFSET;
    float slopeZ = (hForward - hCenter) / NORMAL_SAMPLE_OFFSET;

    // Return XZ slopes (matching BC's getLocalNormals convention)
    return Vec2(slopeX, slopeZ);
}

Vec3 WickedWater::getPosition() const {
    return Vec3(0.0f, tideHeight_, 0.0f);
}

void WickedWater::setVisible(bool visible) {
    visible_ = visible;
    if (weScene) {
        weScene->weather.SetOceanEnabled(visible);
    }
}

bool WickedWater::isValid() const {
    return weScene && weScene->ocean.IsValid();
}

}}} // namespace bc::graphics::wicked

#endif // WITH_WICKED_ENGINE
