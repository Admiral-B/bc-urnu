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

        // Damping ratios
        p.zeta_heave = 0.30f;  // heavily damped by waterplane area
        p.zeta_pitch = (pitchDamping > 0.01f) ? pitchDamping : 0.20f;
        p.zeta_roll  = (rollDamping > 0.01f) ? rollDamping : 0.10f;

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

    // ── Full update step ─────────────────────────────────────────────

    /// Update ship motion from wave surface samples.
    /// Wave readback at bow/stern/port/stbd naturally captures ship-length-vs-
    /// wavelength filtering; the oscillator's frequency response handles the rest.
    inline void update(MotionState& state, const SeakeepingParams& params,
                       float dt, float hCG,
                       float hBow, float hStern,
                       float hPort, float hStbd)
    {
        // Clamp dt to avoid instability
        dt = std::min(dt, 0.1f);
        if (dt < 1e-6f) return;

        // ── Heave: direct wave height at CG ──
        integrateOscillator(state.heave, params.omega_heave, params.zeta_heave,
                           hCG, dt);

        // ── Pitch: wave slope along ship length ──
        // The actual FFT readback at bow/stern already captures ship-length-vs-
        // wavelength filtering naturally (short waves cancel out over the ship's
        // length in the point samples, and the oscillator's natural frequency
        // response attenuates high-frequency excitation).
        float pitchExcitation = std::atan2(hBow - hStern, params.shipLength);

        integrateOscillator(state.pitch, params.omega_pitch, params.zeta_pitch,
                           pitchExcitation, dt);

        // ── Roll: wave slope across ship beam ──
        float rollExcitation = std::atan2(hPort - hStbd, params.shipBreadth);

        integrateOscillator(state.roll, params.omega_roll, params.zeta_roll,
                           rollExcitation, dt);
    }

} // namespace WaveMotion
} // namespace bc

#endif // BC_WAVE_MOTION_MODEL_HPP
