/*   Bridge Command 5.0 Ship Simulator
     Copyright (C) 2026 James Packer

     This program is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License version 2 as
     published by the Free Software Foundation */

#ifndef BC_OCEAN_MATH_HPP
#define BC_OCEAN_MATH_HPP

#include <cmath>
#include <algorithm>

namespace bc {

/// Pure-math ocean utilities that can be tested without WickedEngine.
/// Extracted from WickedWater.cpp and WickedOceanSpectrum.hpp so that
/// Beaufort mapping, spectrum calculations, and cascade blending can
/// be validated in the Catch2 test suite.
namespace OceanMath {

    static constexpr float G      = 9.81f;
    static constexpr float G_CM   = 981.0f;
    static constexpr float PI     = 3.14159265358979f;
    static constexpr float KTS_TO_MPS = 0.5144f;

    // ------------------------------------------------------------------
    //  Beaufort mapping
    // ------------------------------------------------------------------

    /// Beaufort scale wind speed breakpoints (knots, indices 0-12).
    static constexpr float BEAUFORT_WIND_KTS[13] = {
        0, 2, 5, 8.5f, 13, 19, 24, 30, 37, 44, 52, 60, 68
    };

    /// Target significant wave height Hs (meters) for each Beaufort step.
    /// Based on WMO Sea State table. The spectrum is normalized at runtime
    /// to produce exactly this Hs via computeSpectrumScale().
    static constexpr float BEAUFORT_HS[13] = {
    //  B0    B1    B2    B3    B4    B5    B6    B7    B8     B9     B10    B11    B12
        0.0f, 0.1f, 0.3f, 0.6f, 1.0f, 2.0f, 3.0f, 4.0f, 5.5f,  7.0f,  9.0f, 11.5f, 14.0f
    };

    struct OceanParams {
        float targetHs;        ///< Target significant wave height (meters)
        float choppyScale;     ///< WE choppy_scale parameter
        float windSpeedCmps;   ///< Wind speed in cm/s (WE CGS convention)
        float windDirX;        ///< Wind propagation direction X (unit or zero)
        float windDirZ;        ///< Wind propagation direction Z (unit or zero)
        float windSpeedMps;    ///< Wind speed in m/s (for JONSWAP etc.)
    };

    /// Map Beaufort scale + wind parameters to WE ocean parameters.
    /// @param beaufort  Sea state 0.0 - 12.0 (fractional OK)
    /// @param windSpeedKts  Meteorological wind speed in knots
    /// @param windDirectionDeg  Direction wind blows FROM (degrees, met convention)
    inline OceanParams beaufortToOceanParams(float beaufort,
                                              float windSpeedKts,
                                              float windDirectionDeg) {
        OceanParams p{};
        int bi = std::max(0, std::min(12, static_cast<int>(beaufort)));
        float frac = beaufort - bi;
        frac = std::max(0.0f, std::min(1.0f, frac));

        // Interpolate target Hs
        float nextHs = (bi < 12) ? BEAUFORT_HS[bi + 1] : BEAUFORT_HS[12];
        p.targetHs = BEAUFORT_HS[bi] + frac * (nextHs - BEAUFORT_HS[bi]);

        // Choppy scale: lateral displacement multiplier. Controls Jacobian folds
        // (foam/whitecaps). Low at B0-B3 (calm-slight), ramps up from B4.
        // Simplex noise in shader breaks up the repeating grid foam pattern.
        if (beaufort < 3.0f) {
            p.choppyScale = 0.1f + beaufort * 0.1f; // 0.1 at B0, 0.4 at B3
        } else {
            p.choppyScale = std::min(0.4f + (beaufort - 3.0f) * 0.1f, 1.3f);
        }

        // Wind speed: use actual wind, but cap to Beaufort-implied maximum.
        // This prevents a scenario with B1 seas + B4 wind from producing B4 waves.
        // The sea state (Beaufort) is the primary wave driver, not the instantaneous wind.
        float beaufortMaxKts = (bi < 12) ? BEAUFORT_WIND_KTS[bi + 1] : BEAUFORT_WIND_KTS[12];
        float beaufortMaxMps = beaufortMaxKts * KTS_TO_MPS;

        float windMps;
        if (windSpeedKts > 0.1f) {
            windMps = std::min(windSpeedKts * KTS_TO_MPS, beaufortMaxMps);
        } else {
            float nextKts = (bi < 12) ? BEAUFORT_WIND_KTS[bi + 1] : BEAUFORT_WIND_KTS[bi];
            float approxKts = BEAUFORT_WIND_KTS[bi] + frac * (nextKts - BEAUFORT_WIND_KTS[bi]);
            windMps = approxKts * KTS_TO_MPS;
        }
        p.windSpeedMps = windMps;
        // WE wind speed for Phillips spectrum. Capped at 2000 cm/s (20 m/s).
        p.windSpeedCmps = std::max(30.0f, std::min(windMps * 100.0f, 2000.0f));

        // Wind direction: meteorological FROM -> wave propagation WITH (+180 deg)
        float windRad = (windDirectionDeg + 180.0f) * PI / 180.0f;
        if (windMps < 0.3f) {
            p.windDirX = 0.0f;
            p.windDirZ = 1.0f;  // default north if calm
        } else {
            p.windDirX = std::sin(windRad);
            p.windDirZ = std::cos(windRad);
        }

        return p;
    }

    // ------------------------------------------------------------------
    //  Cascade blending weights
    // ------------------------------------------------------------------

    struct CascadeRange {
        float minDistance;   ///< Camera distance where this cascade starts blending in
        float maxDistance;   ///< Camera distance where this cascade is fully visible
    };

    /// Compute blend weight for a single cascade at a given camera distance.
    /// Returns 1.0 when fully visible, 0.0 when out of range.
    /// Linear fade in the transition zones at both ends.
    inline float cascadeWeight(const CascadeRange& range, float cameraDistance) {
        if (cameraDistance < range.minDistance || cameraDistance > range.maxDistance)
            return 0.0f;

        float weight = 1.0f;

        // Fade in at min edge (first 20% of range)
        float fadeIn = range.minDistance + (range.maxDistance - range.minDistance) * 0.2f;
        if (range.minDistance > 0.0f && cameraDistance < fadeIn) {
            weight *= (cameraDistance - range.minDistance) / (fadeIn - range.minDistance);
        }

        // Fade out at max edge (last 20% of range)
        float fadeOut = range.maxDistance - (range.maxDistance - range.minDistance) * 0.2f;
        if (cameraDistance > fadeOut) {
            weight *= (range.maxDistance - cameraDistance) / (range.maxDistance - fadeOut);
        }

        return std::max(0.0f, std::min(1.0f, weight));
    }

    /// Compute normalized blend weights for all cascades.
    /// @param ranges  Array of cascade ranges (size N)
    /// @param weights Output array (size N), will be normalized to sum=1
    /// @param N       Number of cascades
    /// @param cameraDistance  Distance from camera to query point
    inline void cascadeWeights(const CascadeRange* ranges, float* weights,
                                int N, float cameraDistance) {
        float sum = 0.0f;
        for (int i = 0; i < N; i++) {
            weights[i] = cascadeWeight(ranges[i], cameraDistance);
            sum += weights[i];
        }
        if (sum > 0.0f) {
            for (int i = 0; i < N; i++) weights[i] /= sum;
        } else {
            // Fallback: if no cascade covers this distance, use cascade 1 (primary)
            for (int i = 0; i < N; i++) weights[i] = 0.0f;
            if (N > 1) weights[1] = 1.0f;
            else if (N > 0) weights[0] = 1.0f;
        }
    }

    // ------------------------------------------------------------------
    //  JONSWAP spectrum (pure math, no WE dependency)
    // ------------------------------------------------------------------

    /// JONSWAP spectral density S(omega) in m^2*s.
    /// @param omega  Angular frequency (rad/s)
    /// @param windSpeedMps  Wind at 10m height (m/s)
    /// @param fetchKm  Fetch length (km)
    /// @param gamma  Peak enhancement (3.3 standard)
    inline float jonswap(float omega, float windSpeedMps,
                          float fetchKm = 100.0f, float gamma = 3.3f) {
        if (omega < 1e-6f || windSpeedMps < 0.1f) return 0.0f;

        float fetchM = fetchKm * 1000.0f;
        float fetchTilde = G * fetchM / (windSpeedMps * windSpeedMps);
        float omega_p = 22.0f * std::pow(G * G / (windSpeedMps * fetchM), 1.0f / 3.0f);
        float alpha = 0.076f * std::pow(fetchTilde, -0.22f);
        float sigma = (omega <= omega_p) ? 0.07f : 0.09f;
        float r_exp = -((omega - omega_p) * (omega - omega_p)) /
                      (2.0f * sigma * sigma * omega_p * omega_p);
        float r = std::exp(r_exp);
        float pm = alpha * G * G / std::pow(omega, 5.0f) *
                   std::exp(-1.25f * std::pow(omega_p / omega, 4.0f));
        return pm * std::pow(gamma, r);
    }

    /// TMA spectrum: depth-dependent modification of JONSWAP.
    inline float tma(float omega, float windSpeedMps, float depth,
                     float fetchKm = 100.0f) {
        float S = jonswap(omega, windSpeedMps, fetchKm);
        float omega_h = omega * std::sqrt(depth / G);
        float phi;
        if (omega_h <= 1.0f)
            phi = 0.5f * omega_h * omega_h;
        else if (omega_h < 2.0f)
            phi = 1.0f - 0.5f * (2.0f - omega_h) * (2.0f - omega_h);
        else
            phi = 1.0f;
        return S * phi;
    }

    /// Phillips spectrum (WE default, for comparison).
    inline float phillips(float k2, float kDotW, float windSpeedCmps,
                           float amplitude, float dirDepend) {
        if (k2 < 1e-12f) return 0.0f;
        float L = windSpeedCmps * windSpeedCmps / G_CM;
        float damping = L / 1000.0f;
        float phil = amplitude * std::exp(-1.0f / (L * L * k2))
                     / (k2 * k2 * k2) * (kDotW * kDotW);
        if (kDotW < 0) phil *= dirDepend;
        return phil * std::exp(-k2 * damping * damping);
    }

    // ------------------------------------------------------------------
    //  JONSWAP recommended fetch from Beaufort
    // ------------------------------------------------------------------

    /// Estimate fetch from Beaufort scale for JONSWAP spectrum.
    /// Harbour (B0-3) = short fetch, coastal (B4-7) = medium, open sea (B8+) = long.
    inline float beaufortToFetchKm(float beaufort) {
        if (beaufort <= 3.0f) return 10.0f + beaufort * 10.0f;  // 10-40 km
        if (beaufort <= 7.0f) return 40.0f + (beaufort - 3.0f) * 40.0f;  // 40-200 km
        return 200.0f + (beaufort - 7.0f) * 60.0f;  // 200-500 km
    }

    /// JONSWAP gamma: sharper peak for fetch-limited, flatter for fully developed.
    inline float beaufortToGamma(float beaufort) {
        if (beaufort <= 7.0f) return 3.3f;  // standard JONSWAP
        return 3.3f - (beaufort - 7.0f) * 0.46f;  // taper to 1.0 at B12
    }

    // ------------------------------------------------------------------
    //  Fetch-limited wave reduction (SPM/CEM)
    // ------------------------------------------------------------------

    /// Compute fetch-limited significant wave height using the
    /// Shore Protection Manual depth-and-fetch formula.
    /// @param windSpeedMps  Wind speed at 10m (m/s)
    /// @param fetchMeters   Effective fetch distance (m)
    /// @param depthMeters   Water depth (m, positive down). Use >100 for deep water.
    /// @return Hs in meters
    inline float fetchLimitedHs(float windSpeedMps, float fetchMeters, float depthMeters = 100.0f) {
        if (windSpeedMps < 0.5f || fetchMeters < 100.0f) return 0.0f;

        // Adjusted wind speed (SPM convention)
        float UA = 0.71f * std::pow(windSpeedMps, 1.23f);
        float UA2 = UA * UA;

        // Depth-limiting term
        float A1 = 0.53f * std::pow(G * depthMeters / UA2, 0.75f);
        float tA1 = std::tanh(A1);

        // Fetch-limiting term
        float A2 = 0.00565f * std::sqrt(G * fetchMeters / UA2);
        float tA2_ratio = std::tanh(A2 / std::max(tA1, 0.001f));

        return (UA2 / G) * 0.283f * tA1 * tA2_ratio;
    }

    /// Fully developed (unlimited fetch) significant wave height.
    /// Pierson-Moskowitz limit: Hs = 0.22 * U10^2 / g
    inline float fullyDevelopedHs(float windSpeedMps) {
        return 0.22f * windSpeedMps * windSpeedMps / G;
    }

    /// Compute wave amplitude reduction factor due to fetch limitation.
    /// Returns a value in [0, 1] that should multiply wave_amplitude.
    /// Since energy ~ amplitude and Hs ~ sqrt(energy), the amplitude
    /// scale is (Hs_local / Hs_open)^2.
    /// @param windSpeedMps  Wind speed at 10m (m/s)
    /// @param fetchMeters   Effective fetch (m)
    /// @param depthMeters   Water depth (m, >100 for deep water)
    inline float fetchReductionFactor(float windSpeedMps, float fetchMeters, float depthMeters = 100.0f) {
        float hsOpen = fullyDevelopedHs(windSpeedMps);
        if (hsOpen < 0.01f) return 1.0f;

        float hsLocal = fetchLimitedHs(windSpeedMps, fetchMeters, depthMeters);
        float ratio = std::min(hsLocal / hsOpen, 1.0f);
        return ratio * ratio; // amplitude scales as Hs^2
    }

    /// Estimate effective fetch using SPM radial method.
    /// Casts 9 rays at 3-degree spacing around wind direction.
    /// @param shipX, shipZ  Ship position in world coords
    /// @param windDirRad    Wind propagation direction (radians, math convention)
    /// @param heightQuery   Returns terrain height at (x,z). Positive = land.
    /// @param stepSize      Ray-march step size in meters (default 200m)
    /// @param maxFetch      Maximum fetch distance (default 300km)
    /// @return Effective fetch in meters
    inline float estimateEffectiveFetch(
        float shipX, float shipZ, float windDirRad,
        const std::function<float(float, float)>& heightQuery,
        float stepSize = 200.0f, float maxFetch = 300000.0f)
    {
        if (!heightQuery) return maxFetch;

        constexpr int NUM_RADIALS = 9;
        constexpr float SPACING_DEG = 3.0f;
        constexpr float DEG_TO_RAD = PI / 180.0f;

        float sumWeighted = 0.0f;
        float sumWeights = 0.0f;

        for (int i = -(NUM_RADIALS / 2); i <= (NUM_RADIALS / 2); i++) {
            float angle = windDirRad + i * SPACING_DEG * DEG_TO_RAD;
            // Wind comes FROM this direction, so fetch is UPWIND
            // (opposite to propagation direction)
            float dirX = -std::sin(angle); // upwind
            float dirZ = -std::cos(angle);

            float fetchDist = maxFetch;
            for (float d = stepSize; d <= maxFetch; d += stepSize) {
                float qx = shipX + dirX * d;
                float qz = shipZ + dirZ * d;
                float h = heightQuery(qx, qz);
                if (h > 0.0f) { // hit land
                    fetchDist = d;
                    break;
                }
            }

            float theta = i * SPACING_DEG * DEG_TO_RAD;
            float weight = std::cos(theta);
            sumWeighted += fetchDist * weight;
            sumWeights += weight;
        }

        return (sumWeights > 0.0f) ? sumWeighted / sumWeights : maxFetch;
    }

} // namespace OceanMath
} // namespace bc

#endif // BC_OCEAN_MATH_HPP
