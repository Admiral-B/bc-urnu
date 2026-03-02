#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <cmath>

#include "WaveMotionModel.hpp"

using Catch::Approx;
using namespace bc::WaveMotion;

// ── Beaufort to Hs mapping ──────────────────────────────────────────────────

TEST_CASE("Beaufort 0 gives zero Hs", "[wavemotion][beaufort]") {
    REQUIRE(beaufortToHs(0.0f) == Approx(0.0f));
}

TEST_CASE("Beaufort 12 gives ~14m Hs", "[wavemotion][beaufort]") {
    REQUIRE(beaufortToHs(12.0f) == Approx(14.0f));
}

TEST_CASE("Beaufort Hs is monotonically increasing", "[wavemotion][beaufort]") {
    float prevHs = -1.0f;
    for (int b = 0; b <= 12; b++) {
        float hs = beaufortToHs(static_cast<float>(b));
        REQUIRE(hs >= prevHs);
        prevHs = hs;
    }
}

TEST_CASE("Beaufort Hs clamps at boundaries", "[wavemotion][beaufort]") {
    REQUIRE(beaufortToHs(-5.0f) == Approx(0.0f));
    REQUIRE(beaufortToHs(20.0f) == Approx(14.0f));
}

// ── Dominant wavelength ─────────────────────────────────────────────────────

TEST_CASE("Zero wind gives zero wavelength", "[wavemotion][wavelength]") {
    REQUIRE(dominantWavelength(0.0f) == Approx(0.0f).margin(0.01f));
}

TEST_CASE("10 m/s wind gives ~64m wavelength", "[wavemotion][wavelength]") {
    float lambda = dominantWavelength(10.0f);
    float expected = 2 * PI * 100.0f / G;  // ~64m
    REQUIRE(lambda == Approx(expected).margin(1.0f));
}

TEST_CASE("High wind wavelength capped at patch_length", "[wavemotion][wavelength]") {
    // 20 m/s wind -> lambda = 2*PI*400/9.81 = ~256m, capped at 250
    REQUIRE(dominantWavelength(20.0f, 250.0f) == Approx(250.0f));
}

// ── Seakeeping parameters ───────────────────────────────────────────────────

TEST_CASE("Seakeeping params from dimensions", "[wavemotion][seakeeping]") {
    auto p = computeFromDimensions(100.0f, 15.0f, 5.0f, 0, 0, 0, 0, 0);

    // GM auto-estimate: B * 0.06 = 0.9m
    REQUIRE(p.GM == Approx(0.9f));

    // Heave period: 2*PI*sqrt(2*5/9.81) = ~6.35s
    float T_heave = TWO_PI / p.omega_heave;
    REQUIRE(T_heave == Approx(TWO_PI * std::sqrt(10.0f / G)).margin(0.1f));

    // Default damping
    REQUIRE(p.zeta_heave == Approx(0.30f));
    REQUIRE(p.zeta_pitch == Approx(0.20f));
    REQUIRE(p.zeta_roll == Approx(0.10f));
}

TEST_CASE("Seakeeping uses boat.ini periods when given", "[wavemotion][seakeeping]") {
    auto p = computeFromDimensions(100.0f, 15.0f, 5.0f, 10.0f, 8.0f, 1.5f, 0.12f, 0.25f);

    REQUIRE(p.GM == Approx(1.5f));
    float T_roll = TWO_PI / p.omega_roll;
    REQUIRE(T_roll == Approx(10.0f));
    float T_pitch = TWO_PI / p.omega_pitch;
    REQUIRE(T_pitch == Approx(8.0f));
    REQUIRE(p.zeta_roll == Approx(0.12f));
    REQUIRE(p.zeta_pitch == Approx(0.25f));
}

// ── Oscillator convergence ──────────────────────────────────────────────────

TEST_CASE("Oscillator converges to steady excitation", "[wavemotion][oscillator]") {
    DOFState s{0.0f, 0.0f};
    float omega_n = TWO_PI / 8.0f;  // 8s period
    float zeta = 0.3f;
    float excitation = 1.0f;

    // Integrate for 60 seconds at 100Hz -- should converge to excitation
    for (int i = 0; i < 6000; i++) {
        integrateOscillator(s, omega_n, zeta, excitation, 0.01f);
    }

    REQUIRE(s.pos == Approx(excitation).margin(0.05f));
    REQUIRE(s.vel == Approx(0.0f).margin(0.01f));
}

TEST_CASE("Oscillator shows resonance amplification", "[wavemotion][oscillator]") {
    // Drive at natural frequency -- should overshoot steady state
    float omega_n = TWO_PI / 8.0f;
    float zeta = 0.10f;  // lightly damped

    DOFState s{0.0f, 0.0f};
    float maxPos = 0.0f;

    for (int i = 0; i < 3000; i++) {
        float t = i * 0.01f;
        float excitation = std::sin(omega_n * t);  // drive at resonance
        integrateOscillator(s, omega_n, zeta, excitation, 0.01f);
        if (std::abs(s.pos) > maxPos) maxPos = std::abs(s.pos);
    }

    // With zeta=0.10, amplification factor Q = 1/(2*zeta) = 5
    // The oscillator should exceed the excitation amplitude of 1.0
    REQUIRE(maxPos > 1.5f);
}

TEST_CASE("Heavily damped oscillator does not overshoot", "[wavemotion][oscillator]") {
    float omega_n = TWO_PI / 8.0f;
    float zeta = 0.70f;  // critically damped

    DOFState s{0.0f, 0.0f};
    float maxPos = 0.0f;

    for (int i = 0; i < 3000; i++) {
        integrateOscillator(s, omega_n, zeta, 1.0f, 0.01f);
        if (s.pos > maxPos) maxPos = s.pos;
    }

    // Should converge without significant overshoot
    REQUIRE(maxPos < 1.15f);
}

// ── Wavelength reduction ────────────────────────────────────────────────────

TEST_CASE("Small ship has reduction near 1.0", "[wavemotion][reduction]") {
    // 15m boat in 100m waves
    float r = wavelengthReduction(15.0f, 100.0f, PI);  // head seas
    REQUIRE(r > 0.8f);
}

TEST_CASE("Large ship has significant reduction", "[wavemotion][reduction]") {
    // 200m ship in 60m waves, head seas
    float r = wavelengthReduction(200.0f, 60.0f, PI);
    REQUIRE(r < 0.3f);
}

TEST_CASE("Following seas reduce beam roll less", "[wavemotion][reduction]") {
    // mu=0 (following): cos(0)=1, maximum reduction
    float rFollow = wavelengthReduction(15.0f, 60.0f, 0.0f);
    // mu=PI/2 (beam): cos(PI/2)=0, no reduction along this axis
    float rBeam = wavelengthReduction(15.0f, 60.0f, PI * 0.5f);
    REQUIRE(rBeam > rFollow);
}

TEST_CASE("Zero wavelength gives zero reduction", "[wavemotion][reduction]") {
    REQUIRE(wavelengthReduction(100.0f, 0.0f, PI) == Approx(0.0f));
}

// ── Added Resistance in Waves ───────────────────────────────────────────────

TEST_CASE("RAW is zero in calm water", "[wavemotion][raw]") {
    REQUIRE(addedResistanceInWaves(0.0f, 15.0f, 100.0f, PI) == Approx(0.0f));
}

TEST_CASE("RAW is maximum in head/following seas, minimum in beam", "[wavemotion][raw]") {
    float rawHead = addedResistanceInWaves(3.0f, 15.0f, 100.0f, PI);    // head seas
    float rawBeam = addedResistanceInWaves(3.0f, 15.0f, 100.0f, PI/2);  // beam seas
    float rawFollow = addedResistanceInWaves(3.0f, 15.0f, 100.0f, 0.0f);// following

    // cos^2(PI) = cos^2(0) = 1, cos^2(PI/2) = 0
    REQUIRE(rawHead > rawBeam);
    REQUIRE(rawFollow == Approx(rawHead).margin(rawHead * 0.01f));  // symmetric
    REQUIRE(rawBeam == Approx(0.0f).margin(1.0f));                  // beam seas ~0
}

TEST_CASE("RAW increases with wave height", "[wavemotion][raw]") {
    float raw2 = addedResistanceInWaves(2.0f, 15.0f, 100.0f, PI);
    float raw5 = addedResistanceInWaves(5.0f, 15.0f, 100.0f, PI);
    REQUIRE(raw5 > raw2);
}

TEST_CASE("RAW is plausible magnitude", "[wavemotion][raw]") {
    // B7: Hs=5.5m, B=15m, L=100m, head seas
    // RAW = 0.25 * 1025 * 9.81 * 30.25 * 15 * 1.0 / sqrt(100)
    // = 0.25 * 1025 * 9.81 * 30.25 * 15 / 10 = ~113 kN
    float raw = addedResistanceInWaves(5.5f, 15.0f, 100.0f, PI);
    REQUIRE(raw > 50000.0f);   // > 50 kN
    REQUIRE(raw < 200000.0f);  // < 200 kN
}

// ── Rudder sea state factor ─────────────────────────────────────────────────

TEST_CASE("Rudder is 100% effective in calm", "[wavemotion][rudder]") {
    REQUIRE(rudderSeaStateFactor(0.0f) == Approx(1.0f));
}

TEST_CASE("Rudder is degraded in rough weather", "[wavemotion][rudder]") {
    float f5 = rudderSeaStateFactor(5.0f / 12.0f);   // ~B5
    REQUIRE(f5 < 1.0f);
    REQUIRE(f5 > 0.5f);

    float f12 = rudderSeaStateFactor(1.0f);  // B12
    REQUIRE(f12 < 0.7f);
    REQUIRE(f12 > 0.5f);
}

// ── Full update integration ─────────────────────────────────────────────────

TEST_CASE("Ship types respond differently to same waves", "[wavemotion][integration]") {
    // Small tug: 15m x 5m x 2m
    auto tugParams = computeFromDimensions(15.0f, 5.0f, 2.0f, 0, 0, 0, 0, 0);
    MotionState tugState{};

    // Large ferry: 200m x 30m x 7m
    auto ferryParams = computeFromDimensions(200.0f, 30.0f, 7.0f, 0, 0, 0, 0, 0);
    MotionState ferryState{};

    // Simulate 30 seconds with sinusoidal wave excitation
    float windSpeedMps = 10.0f;  // ~B5
    float waveDirRad = 0.0f;     // from north
    float headingRad = PI;       // heading south (beam-ish)
    float weather = 0.42f;       // B5

    for (int i = 0; i < 3000; i++) {
        float t = i * 0.01f;
        // Synthetic wave surface (same for both)
        float wave = 2.0f * std::sin(TWO_PI * t / 8.0f);
        float waveBow = 2.0f * std::sin(TWO_PI * t / 8.0f + 0.3f);
        float waveStern = 2.0f * std::sin(TWO_PI * t / 8.0f - 0.3f);
        float wavePort = 2.0f * std::sin(TWO_PI * t / 8.0f + 0.2f);
        float waveStbd = 2.0f * std::sin(TWO_PI * t / 8.0f - 0.2f);

        update(tugState, tugParams, 0.01f, wave, waveBow, waveStern, wavePort, waveStbd,
               windSpeedMps, headingRad, waveDirRad, weather);
        update(ferryState, ferryParams, 0.01f, wave, waveBow, waveStern, wavePort, waveStbd,
               windSpeedMps, headingRad, waveDirRad, weather);
    }

    // Tug should have larger pitch/roll amplitude than ferry
    // (wavelength reduction is much lower for the large ferry)
    // We can't directly compare max amplitudes from the current state,
    // but we can check that the tug's pitch is larger
    // Run a few more cycles and track maximums
    float tugMaxPitch = 0.0f, ferryMaxPitch = 0.0f;
    float tugMaxRoll = 0.0f, ferryMaxRoll = 0.0f;

    for (int i = 3000; i < 5000; i++) {
        float t = i * 0.01f;
        float wave = 2.0f * std::sin(TWO_PI * t / 8.0f);
        float waveBow = 2.0f * std::sin(TWO_PI * t / 8.0f + 0.3f);
        float waveStern = 2.0f * std::sin(TWO_PI * t / 8.0f - 0.3f);
        float wavePort = 2.0f * std::sin(TWO_PI * t / 8.0f + 0.2f);
        float waveStbd = 2.0f * std::sin(TWO_PI * t / 8.0f - 0.2f);

        update(tugState, tugParams, 0.01f, wave, waveBow, waveStern, wavePort, waveStbd,
               windSpeedMps, headingRad, waveDirRad, weather);
        update(ferryState, ferryParams, 0.01f, wave, waveBow, waveStern, wavePort, waveStbd,
               windSpeedMps, headingRad, waveDirRad, weather);

        tugMaxPitch = std::max(tugMaxPitch, std::abs(tugState.pitch.pos));
        ferryMaxPitch = std::max(ferryMaxPitch, std::abs(ferryState.pitch.pos));
        tugMaxRoll = std::max(tugMaxRoll, std::abs(tugState.roll.pos));
        ferryMaxRoll = std::max(ferryMaxRoll, std::abs(ferryState.roll.pos));
    }

    REQUIRE(tugMaxPitch > ferryMaxPitch);
    REQUIRE(tugMaxRoll > ferryMaxRoll);
}
