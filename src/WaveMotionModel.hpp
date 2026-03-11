/*   Bridge Command 5.0 Ship Simulator
     Copyright (C) 2026 James Packer

     This program is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License version 2 as
     published by the Free Software Foundation */

#ifndef BC_WAVE_MOTION_MODEL_HPP
#define BC_WAVE_MOTION_MODEL_HPP

#include <cmath>
#include <algorithm>

// Prevent Windows min/max macro interference
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

namespace bc {

/// Second-order damped harmonic oscillator for ship motion DOFs.
/// Each DOF (heave, pitch, roll) is modelled as:
///   x'' + 2*zeta*omega_n*x' + omega_n^2*x = omega_n^2 * excitation(t)
///
/// This provides physically plausible resonance, phase lag, and
/// damped response without requiring strip theory or panel methods.
namespace WaveMotion {

    static constexpr float G      = 9.81f;
    static constexpr float PI     = 3.14159265358979f;
    static constexpr float TWO_PI = 6.28318530717959f;
    static constexpr float RHO_SW = 1025.0f;  // seawater density kg/m^3

    // ── Beaufort significant wave height (WMO standard) ──────────────

    /// Approximate Hs (m) from Beaufort number.
    /// WMO: B0=0, B3=0.6, B5=2, B7=5.5, B9=9, B12=14
    static constexpr float BEAUFORT_HS[13] = {
        0.0f, 0.1f, 0.3f, 0.6f, 1.0f, 2.0f, 3.0f,
        5.5f, 7.5f, 9.0f, 11.5f, 13.0f, 14.0f
    };

    /// Interpolate Hs from Beaufort scale (0-12, fractional OK).
    inline float beaufortToHs(float beaufort) {
        beaufort = std::max(0.0f, std::min(12.0f, beaufort));
        int bi = std::min(11, static_cast<int>(beaufort));
        float frac = beaufort - bi;
        return BEAUFORT_HS[bi] + frac * (BEAUFORT_HS[bi + 1] - BEAUFORT_HS[bi]);
    }

    /// Estimate dominant wavelength from wind speed (m/s).
    /// Deep water: lambda = 2*PI*V^2/g, capped at patch_length (250m).
    inline float dominantWavelength(float windSpeedMps, float patchLength = 250.0f) {
        float lambda = TWO_PI * windSpeedMps * windSpeedMps / G;
        return std::min(lambda, patchLength);
    }

    // ── Seakeeping parameters ────────────────────────────────────────

    struct SeakeepingParams {
        // Natural frequencies (rad/s)
        float omega_heave;
        float omega_pitch;
        float omega_roll;

        // Damping ratios (dimensionless)
        float zeta_heave;
        float zeta_pitch;
        float zeta_roll;

        // Ship dimensions
        float shipLength;   // m
        float shipBreadth;  // m
        float shipDraught;  // m

        // Metacentric height
        float GM;           // m
    };

    /// Compute seakeeping parameters from ship dimensions and boat.ini values.
    inline SeakeepingParams computeFromDimensions(
        float L, float B, float T,
        float rollPeriod,    // from boat.ini (s), 0 = auto-estimate
        float pitchPeriod,   // from boat.ini (s), 0 = auto-estimate
        float GM,            // metacentric height (m), 0 = auto-estimate
        float rollDamping,   // damping ratio, 0 = default 0.10
        float pitchDamping)  // damping ratio, 0 = default 0.20
    {
        SeakeepingParams p{};
        p.shipLength = std::max(1.0f, L);
        p.shipBreadth = std::max(1.0f, B);
        p.shipDraught = std::max(0.5f, T);

        // GM: default heuristic B*0.06 (typical for cargo/ferry)
        p.GM = (GM > 0.01f) ? GM : B * 0.06f;

        // Heave: T_heave = 2*PI*sqrt(2*T/g)
        float T_heave = TWO_PI * std::sqrt(2.0f * p.shipDraught / G);
        p.omega_heave = TWO_PI / T_heave;

        // Roll: use boat.ini period if given, else estimate from GM
        float T_roll;
        if (rollPeriod > 1.0f) {
            T_roll = rollPeriod;
        } else {
            // T_roll = 2*PI * 0.38*B / sqrt(g*GM)
            T_roll = TWO_PI * 0.38f * p.shipBreadth / std::sqrt(G * p.GM);
        }
        p.omega_roll = TWO_PI / T_roll;

        // Pitch: use boat.ini period if given, else ~0.8 * T_roll
        float T_pitch;
        if (pitchPeriod > 1.0f) {
            T_pitch = pitchPeriod;
        } else {
            T_pitch = 0.8f * T_roll;
        }
        p.omega_pitch = TWO_PI / T_pitch;

        // Damping ratios: scale with ship size (larger vessels have bilge keels,
        // anti-roll tanks, and more hull form damping). Small boats ~0.08, ferries ~0.25.
        float sizeFactor = std::min(1.0f, p.shipLength / 200.0f); // 0..1 over 0..200m
        p.zeta_heave = 0.30f + sizeFactor * 0.15f;
        p.zeta_pitch = (pitchDamping > 0.01f) ? pitchDamping : (0.20f + sizeFactor * 0.10f);
        p.zeta_roll  = (rollDamping > 0.01f) ? rollDamping : (0.10f + sizeFactor * 0.20f);

        return p;
    }

    // ── Motion state (3 DOF oscillators) ─────────────────────────────

    struct DOFState {
        float pos;  // displacement (m for heave, rad for pitch/roll)
        float vel;  // velocity
    };

    struct MotionState {
        DOFState heave;
        DOFState pitch;
        DOFState roll;
    };

    /// Integrate one DOF using semi-implicit Euler.
    /// excitation is the target displacement from the wave surface.
    inline void integrateOscillator(DOFState& s, float omega_n, float zeta,
                                     float excitation, float dt) {
        // Forcing: omega_n^2 * (excitation - pos) models a spring toward the excitation
        // Full equation: x'' = omega_n^2*(exc - x) - 2*zeta*omega_n*x'
        float acc = omega_n * omega_n * (excitation - s.pos)
                  - 2.0f * zeta * omega_n * s.vel;

        // Semi-implicit Euler: update velocity first, then position
        s.vel += acc * dt;
        s.pos += s.vel * dt;
    }

    // ── Wavelength reduction (Smith-type sinc filter) ────────────────

    /// Ship length vs wavelength reduction.
    /// Large ships bridge over short waves, reducing excitation.
    /// Returns 0..1 multiplier on wave excitation.
    ///
    /// @param L_eff  Effective length (L for pitch, B for roll)
    /// @param lambda Dominant wavelength (m)
    /// @param mu     Heading relative to wave direction (rad), 0=following, PI=head seas
    inline float wavelengthReduction(float L_eff, float lambda, float mu) {
        if (lambda < 0.1f) return 0.0f;  // no waves

        // Component of wavelength along ship axis
        float cosmu = std::cos(mu);
        float arg = PI * L_eff * std::abs(cosmu) / lambda;

        if (arg < 0.01f) return 1.0f;  // wavelength >> ship length

        // sinc function: |sin(arg)/arg|
        return std::abs(std::sin(arg) / arg);
    }

    // ── Added Resistance in Waves (Stawave-1, ITTC) ─────────────────

    /// Compute added resistance force (N) from waves.
    /// Opposes forward motion. Maximum in head seas, minimum in following seas.
    ///
    /// @param Hs        Significant wave height (m)
    /// @param B         Ship breadth (m)
    /// @param L         Ship length (m)
    /// @param mu        Heading relative to wave direction (rad), 0=following, PI=head seas
    /// @return Added resistance force in Newtons (always >= 0)
    inline float addedResistanceInWaves(float Hs, float B, float L, float mu) {
        if (Hs < 0.01f || L < 1.0f) return 0.0f;

        float cosmu = std::cos(mu);
        // RAW = 0.25 * rho * g * Hs^2 * B * cos^2(mu) / sqrt(L)
        float raw = 0.25f * RHO_SW * G * Hs * Hs * B * cosmu * cosmu / std::sqrt(L);
        return std::max(0.0f, raw);
    }

    // ── Rudder sea state degradation ─────────────────────────────────

    /// Rudder effectiveness multiplier in rough seas.
    /// @param beaufort  Beaufort scale 0-12
    /// @return 1.0 at B0, 0.85 at B5, 0.64 at B12
    inline float rudderSeaStateFactor(float beaufort) {
        return 1.0f - 0.03f * std::max(0.0f, std::min(12.0f, beaufort));
    }

    // ── Multi-point buoyancy grid ───────────────────────────────────

    static constexpr int GRID_NX = 5;  // longitudinal sample points
    static constexpr int GRID_NY = 3;  // transverse sample points
    static constexpr int GRID_N  = GRID_NX * GRID_NY; // 15 total

    /// Hull sample point in ship-local coordinates (origin at CG).
    /// x_local: positive forward (bow), y_local: positive port.
    struct HullPoint {
        float x_local;  // m, along ship length from CG
        float y_local;  // m, across ship beam from centerline
    };

    /// Compute the 5x3 grid of hull sample points for buoyancy.
    /// Points are distributed across the waterplane area using an
    /// elliptical footprint (waterplane coefficient Cw ~ 0.8).
    inline void computeHullGrid(const SeakeepingParams& params, HullPoint grid[GRID_N]) {
        float halfL = params.shipLength * 0.5f;
        float halfB = params.shipBreadth * 0.5f;

        int idx = 0;
        for (int ix = 0; ix < GRID_NX; ix++) {
            // Longitudinal: evenly from stern (-halfL) to bow (+halfL)
            float t = (float)ix / (float)(GRID_NX - 1);  // 0..1
            float x_local = -halfL + t * params.shipLength;

            // Elliptical beam at this station: b(x) = halfB * sqrt(1 - (x/halfL)^2)
            // This approximates a typical waterplane shape (widest at midships)
            float xNorm = x_local / halfL;
            float beamHere = halfB * std::sqrt(std::max(0.01f, 1.0f - xNorm * xNorm * 0.6f));

            for (int iy = 0; iy < GRID_NY; iy++) {
                float s = (float)iy / (float)(GRID_NY - 1);  // 0..1
                float y_local = -beamHere + s * 2.0f * beamHere;
                grid[idx].x_local = x_local;
                grid[idx].y_local = y_local;
                idx++;
            }
        }
    }

    /// Multi-point buoyancy result: mean wave height and moments about CG.
    struct BuoyancyResult {
        float meanHeight;      // mean wave elevation across all hull points (m)
        float pitchMoment;     // net pitch excitation (rad) - positive = bow up
        float rollMoment;      // net roll excitation (rad) - positive = port up
    };

    /// Compute heave/pitch/roll excitation from an array of wave heights
    /// sampled at the hull grid points.
    /// @param grid      Hull sample points (ship-local coords)
    /// @param heights   Wave heights at each grid point (world-space, from getWaveHeight)
    /// @param params    Ship seakeeping parameters
    inline BuoyancyResult computeBuoyancy(const HullPoint grid[GRID_N],
                                           const float heights[GRID_N],
                                           const SeakeepingParams& params)
    {
        BuoyancyResult r{};
        float sumH = 0;
        float sumXH = 0;  // moment arm for pitch: sum(x * h)
        float sumYH = 0;  // moment arm for roll: sum(y * h)

        for (int i = 0; i < GRID_N; i++) {
            float h = heights[i];
            sumH += h;
            sumXH += grid[i].x_local * h;
            sumYH += grid[i].y_local * h;
        }

        r.meanHeight = sumH / (float)GRID_N;

        // Pitch excitation: angle from distributed wave pressure
        // Analogous to atan2(moment_arm, inertia_arm) but using the
        // distributed wave slope over the hull waterplane.
        float Ixx = 0;  // second moment of area about y-axis (for pitch)
        float Iyy = 0;  // second moment of area about x-axis (for roll)
        for (int i = 0; i < GRID_N; i++) {
            Ixx += grid[i].x_local * grid[i].x_local;
            Iyy += grid[i].y_local * grid[i].y_local;
        }
        // Avoid division by zero
        if (Ixx > 0.01f) r.pitchMoment = std::atan2(sumXH, Ixx);
        if (Iyy > 0.01f) r.rollMoment  = std::atan2(sumYH, Iyy);

        return r;
    }

    // ── Full update step (5-point, backward compatible) ──────────────

    /// Update ship motion from 5 wave surface samples (legacy interface).
    inline void update(MotionState& state, const SeakeepingParams& params,
                       float dt, float hCG,
                       float hBow, float hStern,
                       float hPort, float hStbd)
    {
        dt = std::min(dt, 0.1f);
        if (dt < 1e-6f) return;

        integrateOscillator(state.heave, params.omega_heave, params.zeta_heave,
                           hCG, dt);

        float pitchExcitation = std::atan2(hBow - hStern, params.shipLength);
        integrateOscillator(state.pitch, params.omega_pitch, params.zeta_pitch,
                           pitchExcitation, dt);

        float rollExcitation = std::atan2(hPort - hStbd, params.shipBreadth);
        integrateOscillator(state.roll, params.omega_roll, params.zeta_roll,
                           rollExcitation, dt);
    }

    // ── Full update step (15-point multi-point buoyancy) ─────────────

    /// Update ship motion from distributed buoyancy sampling.
    /// Provides more accurate pitch/roll excitation than 5-point sampling,
    /// especially for parametric rolling (beam seas) and bow slamming.
    inline void updateMultiPoint(MotionState& state, const SeakeepingParams& params,
                                  float dt, const HullPoint grid[GRID_N],
                                  const float heights[GRID_N],
                                  float windSpeedMps = 10.0f, float headingRelWaveRad = 0.0f)
    {
        dt = std::min(dt, 0.1f);
        if (dt < 1e-6f) return;

        BuoyancyResult buoy = computeBuoyancy(grid, heights, params);

        // Large ships bridge over short waves: reduce excitation by
        // the ratio of ship dimension to dominant wavelength.
        // Pitch uses ship length, roll uses ship breadth.
        float lambda = dominantWavelength(windSpeedMps, 1000.0f);
        float heaveReduction = wavelengthReduction(params.shipLength, lambda, headingRelWaveRad);
        float pitchReduction = wavelengthReduction(params.shipLength, lambda, headingRelWaveRad);
        // Roll: beam seas (mu=PI/2) are worst, use breadth as effective length
        float rollReduction = wavelengthReduction(params.shipBreadth, lambda, headingRelWaveRad + PI * 0.5f);

        integrateOscillator(state.heave, params.omega_heave, params.zeta_heave,
                           buoy.meanHeight * heaveReduction, dt);

        integrateOscillator(state.pitch, params.omega_pitch, params.zeta_pitch,
                           buoy.pitchMoment * pitchReduction, dt);

        integrateOscillator(state.roll, params.omega_roll, params.zeta_roll,
                           buoy.rollMoment * rollReduction, dt);
    }

} // namespace WaveMotion
} // namespace bc

#endif // BC_WAVE_MOTION_MODEL_HPP
