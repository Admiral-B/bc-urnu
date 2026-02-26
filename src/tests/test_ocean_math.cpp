#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <cmath>

#include "OceanMath.hpp"

using Catch::Approx;
using namespace bc::OceanMath;

// ── Beaufort-to-ocean-params mapping ────────────────────────────────────────

TEST_CASE("Beaufort 0 gives minimal waves", "[ocean][beaufort]") {
    auto p = beaufortToOceanParams(0.0f, 0.0f, 0.0f);
    REQUIRE(p.waveAmplitude == Approx(2.0f));
    REQUIRE(p.choppyScale == Approx(0.4f));
    // Calm: wind direction defaults to north
    REQUIRE(p.windDirZ == Approx(1.0f).margin(0.01f));
}

TEST_CASE("Beaufort 12 gives maximum waves", "[ocean][beaufort]") {
    auto p = beaufortToOceanParams(12.0f, 68.0f, 180.0f);
    REQUIRE(p.waveAmplitude == Approx(410.0f));
    REQUIRE(p.choppyScale == Approx(0.8f));  // capped to prevent foam carpet
    REQUIRE(p.windSpeedCmps == Approx(1500.0f));  // capped for Phillips spectrum
}

TEST_CASE("Beaufort interpolation is linear between steps", "[ocean][beaufort]") {
    auto p3 = beaufortToOceanParams(3.0f, 8.5f, 0.0f);
    auto p4 = beaufortToOceanParams(4.0f, 13.0f, 0.0f);
    auto p35 = beaufortToOceanParams(3.5f, 10.0f, 0.0f);

    float expected_amp = (BEAUFORT_AMPLITUDE[3] + BEAUFORT_AMPLITUDE[4]) / 2.0f;
    REQUIRE(p35.waveAmplitude == Approx(expected_amp).margin(1.0f));
    REQUIRE(p35.waveAmplitude > p3.waveAmplitude);
    REQUIRE(p35.waveAmplitude < p4.waveAmplitude);
}

TEST_CASE("Beaufort amplitude is monotonically increasing", "[ocean][beaufort]") {
    float prevAmp = 0.0f;
    for (int b = 0; b <= 12; b++) {
        auto p = beaufortToOceanParams(static_cast<float>(b),
                                        BEAUFORT_WIND_KTS[b], 0.0f);
        REQUIRE(p.waveAmplitude > prevAmp);
        prevAmp = p.waveAmplitude;
    }
}

TEST_CASE("Wind direction FROM converts to propagation WITH", "[ocean][beaufort]") {
    // Wind FROM north (0 deg) -> waves propagate south (180 deg) -> dirZ = -1
    auto pN = beaufortToOceanParams(5.0f, 19.0f, 0.0f);
    REQUIRE(pN.windDirX == Approx(0.0f).margin(0.01f));
    REQUIRE(pN.windDirZ == Approx(-1.0f).margin(0.01f));

    // Wind FROM south (180 deg) -> waves propagate north (0 deg) -> dirZ = +1
    auto pS = beaufortToOceanParams(5.0f, 19.0f, 180.0f);
    REQUIRE(pS.windDirX == Approx(0.0f).margin(0.01f));
    REQUIRE(pS.windDirZ == Approx(1.0f).margin(0.01f));

    // Wind FROM east (90 deg) -> waves propagate west (270 deg) -> dirX = -1
    auto pE = beaufortToOceanParams(5.0f, 19.0f, 90.0f);
    REQUIRE(pE.windDirX == Approx(-1.0f).margin(0.05f));
    REQUIRE(pE.windDirZ == Approx(0.0f).margin(0.05f));
}

TEST_CASE("Wind speed has 30 cm/s floor", "[ocean][beaufort]") {
    auto p = beaufortToOceanParams(0.0f, 0.1f, 0.0f);
    REQUIRE(p.windSpeedCmps >= 30.0f);
}

TEST_CASE("Beaufort clamped to 0-12 range", "[ocean][beaufort]") {
    auto pNeg = beaufortToOceanParams(-1.0f, 0.0f, 0.0f);
    REQUIRE(pNeg.waveAmplitude == Approx(2.0f));  // B0

    auto pOver = beaufortToOceanParams(15.0f, 68.0f, 0.0f);
    REQUIRE(pOver.waveAmplitude == Approx(410.0f));  // B12 max
}

// ── Cascade blending weights ────────────────────────────────────────────────

TEST_CASE("Cascade weight is 1.0 in center of range", "[ocean][cascade]") {
    CascadeRange range{100.0f, 1000.0f};
    float w = cascadeWeight(range, 550.0f);
    REQUIRE(w == Approx(1.0f));
}

TEST_CASE("Cascade weight is 0.0 outside range", "[ocean][cascade]") {
    CascadeRange range{100.0f, 1000.0f};
    REQUIRE(cascadeWeight(range, 50.0f) == Approx(0.0f));
    REQUIRE(cascadeWeight(range, 1500.0f) == Approx(0.0f));
}

TEST_CASE("Cascade weight fades at edges", "[ocean][cascade]") {
    CascadeRange range{100.0f, 1000.0f};
    // At minDistance: weight = 0
    REQUIRE(cascadeWeight(range, 100.0f) == Approx(0.0f));
    // Just inside: small positive weight
    float w = cascadeWeight(range, 120.0f);
    REQUIRE(w > 0.0f);
    REQUIRE(w < 1.0f);
    // At maxDistance: weight = 0
    REQUIRE(cascadeWeight(range, 1000.0f) == Approx(0.0f));
}

TEST_CASE("Cascade weight with minDistance=0 starts at full", "[ocean][cascade]") {
    CascadeRange range{0.0f, 200.0f};
    // No fade-in when min=0
    REQUIRE(cascadeWeight(range, 0.0f) > 0.0f);
    REQUIRE(cascadeWeight(range, 50.0f) == Approx(1.0f));
}

TEST_CASE("Three-cascade weights sum to 1.0", "[ocean][cascade]") {
    // Roadmap cascade ranges
    CascadeRange ranges[3] = {
        {200.0f, 5000.0f},  // Far swells
        {0.0f, 1000.0f},    // Wind waves
        {0.0f, 200.0f},     // Ripples
    };
    float weights[3];

    // Near camera: ripples + wind waves dominate
    cascadeWeights(ranges, weights, 3, 50.0f);
    REQUIRE(weights[0] + weights[1] + weights[2] == Approx(1.0f).margin(0.001f));
    REQUIRE(weights[2] > 0.0f);  // ripples visible
    REQUIRE(weights[1] > 0.0f);  // wind waves visible
    REQUIRE(weights[0] == Approx(0.0f));  // far swells not visible

    // Mid distance: wind waves dominate
    cascadeWeights(ranges, weights, 3, 500.0f);
    REQUIRE(weights[0] + weights[1] + weights[2] == Approx(1.0f).margin(0.001f));
    REQUIRE(weights[1] > weights[2]);  // wind waves > ripples

    // Far distance: swells dominate
    cascadeWeights(ranges, weights, 3, 3000.0f);
    REQUIRE(weights[0] + weights[1] + weights[2] == Approx(1.0f).margin(0.001f));
    REQUIRE(weights[0] > 0.5f);  // far swells dominant
}

TEST_CASE("Fallback when no cascade covers distance", "[ocean][cascade]") {
    CascadeRange ranges[3] = {
        {200.0f, 500.0f},
        {0.0f, 100.0f},
        {0.0f, 50.0f},
    };
    float weights[3];
    // Distance 150 is between cascade 1 (max 100) and cascade 0 (min 200)
    cascadeWeights(ranges, weights, 3, 150.0f);
    // Should fall back to cascade 1
    REQUIRE(weights[1] == Approx(1.0f));
}

// ── JONSWAP spectrum ────────────────────────────────────────────────────────

TEST_CASE("JONSWAP returns 0 at omega=0", "[ocean][spectrum]") {
    REQUIRE(jonswap(0.0f, 10.0f) == Approx(0.0f));
}

TEST_CASE("JONSWAP returns 0 at zero wind", "[ocean][spectrum]") {
    REQUIRE(jonswap(1.0f, 0.0f) == Approx(0.0f));
}

TEST_CASE("JONSWAP has a peak near omega_p", "[ocean][spectrum]") {
    float windMps = 10.0f;
    float fetchKm = 100.0f;

    // Find approximate peak by scanning
    float maxS = 0.0f;
    float peakOmega = 0.0f;
    for (float omega = 0.1f; omega < 5.0f; omega += 0.01f) {
        float S = jonswap(omega, windMps, fetchKm);
        if (S > maxS) {
            maxS = S;
            peakOmega = omega;
        }
    }

    REQUIRE(maxS > 0.0f);

    // Empirical omega_p for 10 m/s, 100km fetch
    float fetchM = fetchKm * 1000.0f;
    float omega_p = 22.0f * std::pow(G * G / (windMps * fetchM), 1.0f / 3.0f);

    // Peak should be near omega_p (within 20%)
    REQUIRE(peakOmega == Approx(omega_p).margin(omega_p * 0.2f));
}

TEST_CASE("JONSWAP energy increases with wind speed", "[ocean][spectrum]") {
    // Integrate total energy: integral S(omega) dOmega approximated by sum
    auto totalEnergy = [](float windMps) {
        float sum = 0.0f;
        for (float omega = 0.1f; omega < 5.0f; omega += 0.01f) {
            sum += jonswap(omega, windMps, 100.0f) * 0.01f;
        }
        return sum;
    };
    REQUIRE(totalEnergy(15.0f) > totalEnergy(10.0f));
    REQUIRE(totalEnergy(10.0f) > totalEnergy(5.0f));
}

TEST_CASE("JONSWAP with higher gamma gives sharper peak", "[ocean][spectrum]") {
    float windMps = 10.0f;
    float fetchKm = 100.0f;

    // gamma=1 (PM) vs gamma=3.3 (JONSWAP standard) vs gamma=7 (very peaked)
    float peakPM = 0.0f, peakJS = 0.0f, peakSharp = 0.0f;
    for (float omega = 0.1f; omega < 5.0f; omega += 0.01f) {
        peakPM = std::max(peakPM, jonswap(omega, windMps, fetchKm, 1.0f));
        peakJS = std::max(peakJS, jonswap(omega, windMps, fetchKm, 3.3f));
        peakSharp = std::max(peakSharp, jonswap(omega, windMps, fetchKm, 7.0f));
    }
    REQUIRE(peakSharp > peakJS);
    REQUIRE(peakJS > peakPM);
}

// ── TMA spectrum (shallow water) ────────────────────────────────────────────

TEST_CASE("TMA equals JONSWAP in deep water", "[ocean][spectrum]") {
    float windMps = 10.0f;
    float omega = 1.0f;
    float deepDepth = 1000.0f;

    float sTMA = tma(omega, windMps, deepDepth);
    float sJON = jonswap(omega, windMps);

    // In deep water, TMA filter phi -> 1.0, so TMA ~= JONSWAP
    REQUIRE(sTMA == Approx(sJON).margin(sJON * 0.01f));
}

TEST_CASE("TMA attenuates in shallow water", "[ocean][spectrum]") {
    float windMps = 10.0f;
    float omega = 1.0f;

    float sDeep = tma(omega, windMps, 100.0f);
    float sShallow = tma(omega, windMps, 5.0f);

    REQUIRE(sShallow <= sDeep);
}

TEST_CASE("TMA phi is 0 at zero depth", "[ocean][spectrum]") {
    // omega_h = omega * sqrt(0/g) = 0 -> phi = 0
    float s = tma(1.0f, 10.0f, 0.001f);
    REQUIRE(s < 0.01f);
}

// ── Beaufort fetch/gamma mapping ────────────────────────────────────────────

TEST_CASE("Fetch increases with Beaufort", "[ocean][spectrum]") {
    REQUIRE(beaufortToFetchKm(0.0f) < beaufortToFetchKm(5.0f));
    REQUIRE(beaufortToFetchKm(5.0f) < beaufortToFetchKm(10.0f));
}

TEST_CASE("Gamma is 3.3 for moderate sea states", "[ocean][spectrum]") {
    REQUIRE(beaufortToGamma(3.0f) == Approx(3.3f));
    REQUIRE(beaufortToGamma(5.0f) == Approx(3.3f));
}

TEST_CASE("Gamma approaches 1.0 at Beaufort 12", "[ocean][spectrum]") {
    float g12 = beaufortToGamma(12.0f);
    REQUIRE(g12 >= 0.9f);
    REQUIRE(g12 <= 1.1f);
}

// ── Phillips spectrum ───────────────────────────────────────────────────────

TEST_CASE("Phillips returns 0 at k=0", "[ocean][spectrum]") {
    REQUIRE(phillips(0.0f, 1.0f, 600.0f, 300.0f, 0.07f) == Approx(0.0f));
}

TEST_CASE("Phillips is attenuated against wind direction", "[ocean][spectrum]") {
    float k2 = 0.01f;
    float windSpeed = 600.0f;
    float amp = 300.0f;
    float dirDep = 0.07f;

    float withWind = phillips(k2, 0.1f, windSpeed, amp, dirDep);
    float againstWind = phillips(k2, -0.1f, windSpeed, amp, dirDep);

    // Against wind should be much smaller due to dirDepend
    REQUIRE(withWind > againstWind);
    REQUIRE(againstWind == Approx(withWind * dirDep).margin(withWind * 0.01f));
}
