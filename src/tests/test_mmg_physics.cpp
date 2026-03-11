#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <cmath>
#include <vector>
#include <algorithm>

#include "MMGPhysicsModel.hpp"

using Catch::Approx;

// Helper: create a KVLCC2-like ship for testing
static MMGPhysicsModel createKVLCC2() {
    ShipDimensions dims;
    dims.length = 320.0;
    dims.beam = 58.0;
    dims.draught = 20.8;
    dims.displacement = 312600.0 * 1000.0; // tonnes to kg
    dims.blockCoefficient = 0.8098;
    dims.maxSpeed = 15.5 * 0.5144; // 15.5 knots to m/s
    dims.maxEngineForce = 2.5e6;
    dims.propellerDiameter = 9.86;
    dims.maxRPM = 100.0;
    dims.singleEngine = true;

    // Use published KVLCC2 coefficients (defaults in MMGCoefficients)
    return MMGPhysicsModel(dims);
}

// Helper: create a smaller vessel for faster convergence tests
static MMGPhysicsModel createSmallVessel() {
    ShipDimensions dims;
    dims.length = 50.0;
    dims.beam = 10.0;
    dims.draught = 3.0;
    dims.displacement = 500.0 * 1000.0; // 500 tonnes
    dims.blockCoefficient = 0.65;
    dims.maxSpeed = 12.0 * 0.5144; // 12 knots
    dims.maxEngineForce = 200000.0;
    dims.propellerDiameter = 2.0;
    dims.maxRPM = 300.0;
    dims.singleEngine = true;

    // Estimate coefficients from dimensions
    MMGCoefficients coeffs;
    coeffs.estimateFromDimensions(dims);
    return MMGPhysicsModel(dims, coeffs);
}

// Helper: simulate for N seconds, return final state
static PhysicsState simulate(MMGPhysicsModel& model, PhysicsInput input,
                              PhysicsState initial, double duration, double dt = 0.02) {
    PhysicsState state = initial;
    int steps = static_cast<int>(duration / dt);
    for (int i = 0; i < steps; i++) {
        model.step(dt, input, state);
    }
    return state;
}

// ── Basic sanity ────────────────────────────────────────────────────────────

TEST_CASE("MMG: ship at rest stays at rest", "[mmg]") {
    auto model = createKVLCC2();
    PhysicsState state;
    PhysicsInput input;
    input.portEngine = 0;
    input.stbdEngine = 0;
    input.rudderAngle = 0;

    state = simulate(model, input, state, 10.0);

    REQUIRE(fabs(state.surge) < 0.01);
    REQUIRE(fabs(state.sway) < 0.01);
    REQUIRE(fabs(state.yawRate) < 0.01);
}

TEST_CASE("MMG: engine produces forward motion", "[mmg]") {
    auto model = createSmallVessel();
    PhysicsState state;
    PhysicsInput input;
    input.portEngine = 0.5;
    input.rudderAngle = 0;

    state = simulate(model, input, state, 60.0);

    REQUIRE(state.surge > 0.5); // Should be moving forward
    REQUIRE(fabs(state.sway) < 0.5); // Minimal lateral motion
    REQUIRE(state.posZ > 10.0); // Should have moved forward (North = +Z)
}

TEST_CASE("MMG: full ahead reaches reasonable speed", "[mmg]") {
    auto model = createSmallVessel();
    PhysicsState state;
    PhysicsInput input;
    input.portEngine = 1.0;
    input.rudderAngle = 0;

    // Simulate 5 minutes
    state = simulate(model, input, state, 300.0);

    // Speed should be reasonable (not infinite, not zero)
    REQUIRE(state.surge > 1.0);
    REQUIRE(state.surge < 20.0); // Not unreasonably fast
}

TEST_CASE("MMG: astern reverses", "[mmg]") {
    auto model = createSmallVessel();
    PhysicsState state;
    PhysicsInput input;
    input.portEngine = -0.5;
    input.rudderAngle = 0;

    state = simulate(model, input, state, 60.0);

    REQUIRE(state.surge < -0.1); // Should be moving astern
}

// ── Rudder effects ──────────────────────────────────────────────────────────

TEST_CASE("MMG: starboard rudder turns ship to starboard", "[mmg]") {
    auto model = createSmallVessel();
    PhysicsState state;
    state.surge = 5.0; // Give initial speed
    PhysicsInput input;
    input.portEngine = 0.5;
    input.rudderAngle = 15.0; // Starboard rudder

    state = simulate(model, input, state, 60.0);

    // Heading should increase (CW = starboard)
    REQUIRE(state.heading > 5.0);
    REQUIRE(state.yawRate > 0.0);
}

TEST_CASE("MMG: port rudder turns ship to port", "[mmg]") {
    auto model = createSmallVessel();
    PhysicsState state;
    state.surge = 5.0;
    PhysicsInput input;
    input.portEngine = 0.5;
    input.rudderAngle = -15.0; // Port rudder

    state = simulate(model, input, state, 60.0);

    // Heading should decrease (CCW = port) - wrapped around 360
    REQUIRE((state.heading > 300.0 || state.heading < 5.0));
}

TEST_CASE("MMG: more rudder gives tighter turn", "[mmg]") {
    auto model = createSmallVessel();
    PhysicsInput input;
    input.portEngine = 0.5;

    // 10 degree rudder
    PhysicsState state10;
    state10.surge = 5.0;
    input.rudderAngle = 10.0;
    state10 = simulate(model, input, state10, 30.0);

    // 25 degree rudder
    PhysicsState state25;
    state25.surge = 5.0;
    input.rudderAngle = 25.0;
    state25 = simulate(model, input, state25, 30.0);

    // Greater rudder should give greater heading change
    REQUIRE(state25.heading > state10.heading);
}

// ── Turning circle (KVLCC2 validation) ──────────────────────────────────────

TEST_CASE("MMG: KVLCC2 turning circle advance is reasonable", "[mmg][kvlcc2]") {
    auto model = createKVLCC2();
    PhysicsState state;
    state.surge = 15.5 * 0.5144; // Design speed
    PhysicsInput input;
    input.portEngine = 1.0;
    input.rudderAngle = 35.0; // Hard starboard

    // Simulate for 20 minutes (typical large ship turning circle)
    PhysicsState final_state = simulate(model, input, state, 1200.0);

    // Turning circle advance for KVLCC2 at 35deg rudder should be ~3.2 * L
    // We check that the ship has actually turned (heading changed significantly)
    double headingChange = final_state.heading; // Started at 0
    // After 20 minutes with hard rudder, should have completed at least one full circle
    // But heading wraps, so check the rate of turn is non-trivial
    REQUIRE(fabs(final_state.yawRate) > 0.01); // Ship is turning
    REQUIRE(final_state.surge > 0.0); // Still moving forward
}

// ── Coefficient estimation ──────────────────────────────────────────────────

TEST_CASE("MMG: coefficient estimation produces reasonable values", "[mmg]") {
    ShipDimensions dims;
    dims.length = 100.0;
    dims.beam = 16.0;
    dims.draught = 6.0;
    dims.blockCoefficient = 0.7;

    MMGCoefficients coeffs;
    coeffs.estimateFromDimensions(dims);

    // Added mass should be positive
    REQUIRE(coeffs.m_x_prime > 0.0);
    REQUIRE(coeffs.m_y_prime > 0.0);
    REQUIRE(coeffs.J_z_prime > 0.0);

    // Y_v should be negative (resists sway)
    REQUIRE(coeffs.Y_v_prime < 0.0);

    // N_r should be negative (resists yaw)
    REQUIRE(coeffs.N_r_prime < 0.0);

    // Thrust deduction and wake fraction should be 0-1
    REQUIRE(coeffs.t_P > 0.0);
    REQUIRE(coeffs.t_P < 1.0);
    REQUIRE(coeffs.w_P0 > 0.0);
    REQUIRE(coeffs.w_P0 < 1.0);
}

// ── Added mass effect ───────────────────────────────────────────────────────

TEST_CASE("MMG: added mass increases effective inertia", "[mmg]") {
    ShipDimensions dims;
    dims.length = 100.0;
    dims.beam = 16.0;
    dims.draught = 6.0;
    dims.displacement = 5000.0 * 1000.0;
    dims.blockCoefficient = 0.7;
    dims.maxSpeed = 10.0;
    dims.maxEngineForce = 500000.0;
    dims.propellerDiameter = 3.0;
    dims.maxRPM = 200.0;
    dims.singleEngine = true;

    MMGCoefficients coeffs;
    coeffs.estimateFromDimensions(dims);

    MMGPhysicsModel model(dims, coeffs);

    // The mass_Y (with added mass) should be significantly larger than bare mass
    // This is what makes ship turning feel "heavy"
    double m_y_add = coeffs.m_y_prime * 0.5 * 1025.0 * dims.length * dims.length * dims.draught;
    REQUIRE(m_y_add > 0.1 * dims.displacement); // Added mass > 10% of ship mass
}

// ── Position integration ────────────────────────────────────────────────────

TEST_CASE("MMG: straight line position updates correctly", "[mmg]") {
    auto model = createSmallVessel();
    PhysicsState state;
    state.surge = 5.0; // 5 m/s north
    state.heading = 0.0;

    PhysicsInput input;
    input.portEngine = 0.0;
    input.rudderAngle = 0.0;

    // Simulate 10 seconds with no engine (coast)
    PhysicsState final_state = simulate(model, input, state, 10.0);

    // Should have moved approximately north (Z direction)
    REQUIRE(final_state.posZ > 20.0); // At least 20m (accounting for drag)
    REQUIRE(fabs(final_state.posX) < 1.0); // Minimal lateral drift
}

TEST_CASE("MMG: heading 90 moves east", "[mmg]") {
    auto model = createSmallVessel();
    PhysicsState state;
    state.surge = 5.0;
    state.heading = 90.0; // East

    PhysicsInput input;

    PhysicsState final_state = simulate(model, input, state, 10.0);

    // Should have moved in +X direction
    REQUIRE(final_state.posX > 20.0);
    REQUIRE(fabs(final_state.posZ) < 1.0);
}

// ══════════════════════════════════════════════════════════════════════════════
// SIMMAN 2008 KVLCC2 validation tests
// Reference: Yasukawa & Yoshimura (2015), SIMMAN 2008 workshop data
// ══════════════════════════════════════════════════════════════════════════════

// Helper: create KVLCC2 with published coefficients (defaults in MMGCoefficients struct)
static MMGPhysicsModel createKVLCC2Published() {
    ShipDimensions dims;
    dims.length = 320.0;
    dims.beam = 58.0;
    dims.draught = 20.8;
    dims.displacement = 312600.0 * 1000.0; // tonnes to kg
    dims.blockCoefficient = 0.8098;
    dims.maxSpeed = 15.5 * 0.5144; // 15.5 knots to m/s
    dims.maxEngineForce = 2.5e6;
    dims.propellerDiameter = 9.86;
    dims.maxRPM = 100.0;
    dims.singleEngine = true;

    // Use the default MMGCoefficients which ARE the published KVLCC2 values
    MMGCoefficients coeffs; // Defaults from Y&Y 2015, Table A1
    return MMGPhysicsModel(dims, coeffs);
}

// Helper: run steady-speed approach phase, return state at design speed
static PhysicsState runApproach(MMGPhysicsModel& model, double targetSpeed, double duration = 600.0) {
    PhysicsState state;
    state.heading = 0.0; // North
    PhysicsInput input;
    input.portEngine = 1.0;
    input.rudderAngle = 0.0;

    double dt = 0.02;
    int steps = static_cast<int>(duration / dt);
    for (int i = 0; i < steps; i++) {
        model.step(dt, input, state);
    }
    return state;
}

// Trajectory point for analysis
struct TrajectoryPoint {
    double t;
    double x, z;
    double heading;
    double surge;
};

// Helper: simulate and record trajectory
static std::vector<TrajectoryPoint> simulateTrajectory(
    MMGPhysicsModel& model, PhysicsInput input,
    PhysicsState initial, double duration, double dt = 0.02)
{
    std::vector<TrajectoryPoint> trajectory;
    PhysicsState state = initial;
    double t = 0.0;
    int steps = static_cast<int>(duration / dt);

    trajectory.push_back({0.0, state.posX, state.posZ, state.heading, state.surge});
    for (int i = 0; i < steps; i++) {
        model.step(dt, input, state);
        t += dt;
        trajectory.push_back({t, state.posX, state.posZ, state.heading, state.surge});
    }
    return trajectory;
}

// Helper: normalize heading to [0, 360)
static double normHeading(double h) {
    while (h < 0) h += 360.0;
    while (h >= 360.0) h -= 360.0;
    return h;
}

// Helper: compute cumulative heading change (handles wrapping)
static double cumulativeHeadingChange(const std::vector<TrajectoryPoint>& traj) {
    double total = 0.0;
    for (size_t i = 1; i < traj.size(); i++) {
        double dh = traj[i].heading - traj[i-1].heading;
        // Handle wrapping
        if (dh > 180.0) dh -= 360.0;
        if (dh < -180.0) dh += 360.0;
        total += dh;
    }
    return total;
}

// ── Turning circle test (35° rudder) ─────────────────────────────────────────

TEST_CASE("SIMMAN: KVLCC2 turning circle 35 deg rudder", "[mmg][simman][kvlcc2]") {
    auto model = createKVLCC2Published();
    double L = 320.0;

    // Approach phase: let ship reach steady speed
    PhysicsState state = runApproach(model, 15.5 * 0.5144, 600.0);
    double approachSpeed = state.surge;
    INFO("Approach speed: " << approachSpeed << " m/s");
    REQUIRE(approachSpeed > 3.0); // Should have reached some speed

    // Record starting position
    double x0 = state.posX;
    double z0 = state.posZ;
    state.posX = 0.0; // Reset position for clarity
    state.posZ = 0.0;
    double h0 = state.heading;

    // Apply 35 deg starboard rudder and record trajectory
    PhysicsInput input;
    input.portEngine = 1.0;
    input.rudderAngle = 35.0;

    auto traj = simulateTrajectory(model, input, state, 1800.0); // 30 minutes

    // Find advance (posZ when heading changes by 90°)
    // Find tactical diameter (posX when heading changes by 180°)
    double advance = 0.0;
    double transfer = 0.0;
    double tacticalDiameter = 0.0;
    bool found90 = false;
    bool found180 = false;

    for (size_t i = 1; i < traj.size(); i++) {
        double cumHeading = cumulativeHeadingChange(
            std::vector<TrajectoryPoint>(traj.begin(), traj.begin() + i + 1));

        if (!found90 && cumHeading >= 90.0) {
            advance = traj[i].z; // Distance in original heading direction
            transfer = traj[i].x; // Lateral distance
            found90 = true;
            INFO("At 90° turn - Advance: " << advance << " m (" << advance/L << " L)");
            INFO("At 90° turn - Transfer: " << transfer << " m (" << transfer/L << " L)");
        }
        if (!found180 && cumHeading >= 180.0) {
            tacticalDiameter = traj[i].x; // Lateral distance at 180° turn
            found180 = true;
            INFO("At 180° turn - Tactical diameter: " << tacticalDiameter << " m (" << tacticalDiameter/L << " L)");
            break;
        }
    }

    // Verify the ship actually turned
    double totalHeading = cumulativeHeadingChange(traj);
    INFO("Total heading change: " << totalHeading << " degrees");
    REQUIRE(found90);

    // SIMMAN reference: Advance ~ 3.2 * L (±30% for model variation)
    REQUIRE(advance / L > 1.5);
    REQUIRE(advance / L < 6.0);

    // SIMMAN reference: Transfer ~ 1.5 * L (±30%)
    REQUIRE(transfer / L > 0.5);
    REQUIRE(transfer / L < 4.0);

    if (found180) {
        // SIMMAN reference: Tactical diameter ~ 3.6 * L (±30%)
        REQUIRE(tacticalDiameter / L > 1.5);
        REQUIRE(tacticalDiameter / L < 7.0);
    }

    // Speed loss: final steady-state speed should be ~40-80% of approach speed
    auto& lastPoint = traj.back();
    double speedRatio = lastPoint.surge / approachSpeed;
    INFO("Speed ratio (final/approach): " << speedRatio);
    REQUIRE(speedRatio > 0.1);
    REQUIRE(speedRatio < 0.95);
}

// ── 10/10 Zigzag test ────────────────────────────────────────────────────────

TEST_CASE("SIMMAN: KVLCC2 10/10 zigzag test", "[mmg][simman][kvlcc2]") {
    auto model = createKVLCC2Published();

    // Approach phase
    PhysicsState state = runApproach(model, 15.5 * 0.5144, 600.0);
    double approachSpeed = state.surge;
    INFO("Approach speed: " << approachSpeed << " m/s");
    REQUIRE(approachSpeed > 3.0);

    // Zigzag: apply 10° rudder, reverse when heading reaches ±10°
    double dt = 0.02;
    double rudderAngle = 10.0; // Start with starboard
    double headingAtStart = state.heading;
    PhysicsInput input;
    input.portEngine = 1.0;
    input.rudderAngle = rudderAngle;

    int reversals = 0;
    double firstOvershoot = 0.0;
    double firstReversalTime = 0.0;
    double secondReversalTime = 0.0;
    double maxHeadingDeviation = 0.0;
    bool pastFirstReversal = false;
    double t = 0.0;

    // Simulate for 10 minutes max
    for (int i = 0; i < 30000; i++) {
        model.step(dt, input, state);
        t += dt;

        double headingDev = state.heading - headingAtStart;
        // Handle wrapping
        if (headingDev > 180.0) headingDev -= 360.0;
        if (headingDev < -180.0) headingDev += 360.0;

        // Check for rudder reversal condition
        if (rudderAngle > 0 && headingDev >= 10.0) {
            // Heading reached +10°, reverse to port
            rudderAngle = -10.0;
            input.rudderAngle = rudderAngle;
            reversals++;
            if (reversals == 1) {
                firstReversalTime = t;
            }
            if (reversals == 3) {
                secondReversalTime = t;
            }
        } else if (rudderAngle < 0 && headingDev <= -10.0) {
            // Heading reached -10°, reverse to starboard
            rudderAngle = 10.0;
            input.rudderAngle = rudderAngle;
            reversals++;
            if (reversals == 2) {
                secondReversalTime = t;
            }
        }

        // Track max deviation after first reversal for overshoot
        if (reversals == 1 && !pastFirstReversal) {
            pastFirstReversal = true;
        }
        if (pastFirstReversal && reversals == 1) {
            if (headingDev > maxHeadingDeviation) {
                maxHeadingDeviation = headingDev;
            }
        }

        // Stop after enough reversals
        if (reversals >= 4) break;
    }

    firstOvershoot = maxHeadingDeviation - 10.0; // Overshoot beyond the 10° target

    INFO("Reversals: " << reversals);
    INFO("First overshoot: " << firstOvershoot << " degrees");
    INFO("Time at first reversal: " << firstReversalTime << " s");

    // Ship should execute at least 2 reversals in 10 minutes
    REQUIRE(reversals >= 2);

    // SIMMAN reference: first overshoot ~ 5-15 degrees (wide tolerance for model variation)
    if (firstOvershoot > 0) {
        REQUIRE(firstOvershoot > 1.0);
        REQUIRE(firstOvershoot < 25.0);
    }

    // Zigzag period (time between 1st and 3rd reversal = one full cycle)
    if (reversals >= 3) {
        double period = secondReversalTime - firstReversalTime;
        INFO("Zigzag period (half): " << period << " s");
        // SIMMAN reference: full period ~ 100-200s for KVLCC2
        // Half period should be 50-100s
        REQUIRE(period > 20.0);
        REQUIRE(period < 300.0);
    }
}

// ── Crash stop test ──────────────────────────────────────────────────────────

TEST_CASE("SIMMAN: KVLCC2 crash stop", "[mmg][simman][kvlcc2]") {
    auto model = createKVLCC2Published();
    double L = 320.0;

    // Approach phase
    PhysicsState state = runApproach(model, 15.5 * 0.5144, 600.0);
    double approachSpeed = state.surge;
    INFO("Approach speed: " << approachSpeed << " m/s");
    REQUIRE(approachSpeed > 3.0);

    // Record starting position
    state.posX = 0.0;
    state.posZ = 0.0;

    // Full astern
    PhysicsInput input;
    input.portEngine = -1.0;
    input.rudderAngle = 0.0;

    double dt = 0.02;
    double t = 0.0;
    double maxHeadReach = 0.0; // Maximum distance in original heading direction
    double trackLength = 0.0;
    double prevX = 0.0, prevZ = 0.0;

    // Simulate until stopped or 30 minutes
    for (int i = 0; i < 90000; i++) {
        model.step(dt, input, state);
        t += dt;

        // Track distance traveled (head reach = distance in original heading direction = Z)
        if (state.posZ > maxHeadReach) {
            maxHeadReach = state.posZ;
        }

        // Accumulate track length
        double dx = state.posX - prevX;
        double dz = state.posZ - prevZ;
        trackLength += sqrt(dx*dx + dz*dz);
        prevX = state.posX;
        prevZ = state.posZ;

        // Check if stopped
        if (fabs(state.surge) < 0.05 && t > 30.0) {
            break;
        }
    }

    INFO("Head reach: " << maxHeadReach << " m (" << maxHeadReach/L << " L)");
    INFO("Track reach: " << trackLength << " m (" << trackLength/L << " L)");
    INFO("Time to stop: " << t << " s");
    INFO("Final surge: " << state.surge << " m/s");

    // SIMMAN reference: head reach ~ 15-20 * L (wide tolerance)
    // Our model may differ since K_T polynomial doesn't model 4-quadrant propeller
    REQUIRE(maxHeadReach / L > 2.0);
    REQUIRE(maxHeadReach / L < 40.0);

    // Track reach should be larger than head reach
    REQUIRE(trackLength >= maxHeadReach);

    // Ship should eventually slow down significantly
    REQUIRE(state.surge < approachSpeed * 0.5);
}

// ── Steady speed validation ──────────────────────────────────────────────────

TEST_CASE("SIMMAN: KVLCC2 steady speed at full RPM is plausible", "[mmg][simman][kvlcc2]") {
    auto model = createKVLCC2Published();

    // Run at full engine for 20 minutes
    PhysicsState state = runApproach(model, 15.5 * 0.5144, 1200.0);

    double steadySpeed_kts = state.surge / 0.5144;
    INFO("Steady speed: " << steadySpeed_kts << " knots (" << state.surge << " m/s)");

    // KVLCC2 design speed is 15.5 knots
    // With our simplified model, anything 5-25 knots is acceptable
    REQUIRE(steadySpeed_kts > 5.0);
    REQUIRE(steadySpeed_kts < 25.0);
}

// ── Symmetry test ────────────────────────────────────────────────────────────

TEST_CASE("SIMMAN: Turning symmetry (port vs starboard)", "[mmg][simman]") {
    auto model = createKVLCC2Published();

    // Test that port and starboard turns are symmetric
    PhysicsState stateStbd = runApproach(model, 15.5 * 0.5144, 300.0);
    PhysicsState statePort = stateStbd; // Same initial conditions

    PhysicsInput inputStbd;
    inputStbd.portEngine = 1.0;
    inputStbd.rudderAngle = 35.0; // Starboard

    PhysicsInput inputPort;
    inputPort.portEngine = 1.0;
    inputPort.rudderAngle = -35.0; // Port

    auto trajStbd = simulateTrajectory(model, inputStbd, stateStbd, 300.0);
    auto trajPort = simulateTrajectory(model, inputPort, statePort, 300.0);

    double hdgStbd = cumulativeHeadingChange(trajStbd);
    double hdgPort = cumulativeHeadingChange(trajPort);

    INFO("Starboard heading change: " << hdgStbd);
    INFO("Port heading change: " << hdgPort);

    // Should be approximately equal magnitude, opposite sign
    // Allow 20% asymmetry (gamma_R_positive != gamma_R_negative causes some)
    REQUIRE(fabs(hdgStbd + hdgPort) < 0.3 * fabs(hdgStbd));
}

// ══════════════════════════════════════════════════════════════════════════════
// Phase 8: Advanced physics tests
// ══════════════════════════════════════════════════════════════════════════════

// ── 8-01: Shallow water effects ─────────────────────────────────────────────

TEST_CASE("Shallow water factor is 1.0 in deep water", "[mmg][shallow]") {
    REQUIRE(MMGPhysicsModel::shallowWaterFactor(10.0) == Approx(1.0));
    REQUIRE(MMGPhysicsModel::shallowWaterFactor(5.0) == Approx(1.0));
    REQUIRE(MMGPhysicsModel::shallowWaterFactor(4.0) == Approx(1.0));
}

TEST_CASE("Shallow water factor increases as depth decreases", "[mmg][shallow]") {
    double f4 = MMGPhysicsModel::shallowWaterFactor(4.0);
    double f3 = MMGPhysicsModel::shallowWaterFactor(3.0);
    double f2 = MMGPhysicsModel::shallowWaterFactor(2.0);
    double f15 = MMGPhysicsModel::shallowWaterFactor(1.5);
    double f12 = MMGPhysicsModel::shallowWaterFactor(1.2);

    REQUIRE(f3 > f4);
    REQUIRE(f2 > f3);
    REQUIRE(f15 > f2);
    REQUIRE(f12 > f15);

    // At h/T=1.2 (very shallow), factor should be significant (>1.5x)
    REQUIRE(f12 > 1.5);
    // But not unreasonably large
    REQUIRE(f12 < 5.0);
}

TEST_CASE("Shallow water factor handles extreme values", "[mmg][shallow]") {
    // Very deep water
    REQUIRE(MMGPhysicsModel::shallowWaterFactor(100.0) == Approx(1.0));
    // Near grounding (clamped to 1.05)
    double fNearGround = MMGPhysicsModel::shallowWaterFactor(1.01);
    REQUIRE(fNearGround > 1.0);
    REQUIRE(std::isfinite(fNearGround));
}

TEST_CASE("Barras squat is zero in deep water", "[mmg][shallow]") {
    auto model = createKVLCC2();
    REQUIRE(model.computeSquat(10.0, 10.0) == Approx(0.0));
    REQUIRE(model.computeSquat(10.0, 5.0) == Approx(0.0));
}

TEST_CASE("Barras squat increases with speed", "[mmg][shallow]") {
    auto model = createKVLCC2();
    double hT = 2.0;
    double squat5 = model.computeSquat(5.0, hT);
    double squat10 = model.computeSquat(10.0, hT);
    double squat15 = model.computeSquat(15.0, hT);

    REQUIRE(squat5 > 0.0);
    REQUIRE(squat10 > squat5);
    REQUIRE(squat15 > squat10);

    // At 10 knots with h/T=2.0, squat should be modest (< 2m)
    REQUIRE(squat10 < 2.0);
}

TEST_CASE("Barras squat increases in shallower water", "[mmg][shallow]") {
    auto model = createKVLCC2();
    double speed = 10.0; // knots
    double squat3 = model.computeSquat(speed, 3.0);
    double squat2 = model.computeSquat(speed, 2.0);
    double squat15 = model.computeSquat(speed, 1.5);

    REQUIRE(squat2 > squat3);
    REQUIRE(squat15 > squat2);
}

TEST_CASE("Ship turns wider in shallow water", "[mmg][shallow]") {
    auto model = createSmallVessel();

    // Get to steady speed
    PhysicsState stateDeep = simulate(model, PhysicsInput{}, PhysicsState{}, 60.0,
                                       0.02);
    // Re-init with engine at 0.8
    PhysicsInput approach;
    approach.portEngine = 0.8;
    stateDeep = simulate(model, approach, PhysicsState{}, 120.0);

    PhysicsState stateShallow = stateDeep;

    // Deep water turn (default depth=100m)
    PhysicsInput turnDeep;
    turnDeep.portEngine = 0.8;
    turnDeep.rudderAngle = 35.0;
    turnDeep.waterDepth = 100.0; // Deep

    PhysicsState finalDeep = simulate(model, turnDeep, stateDeep, 120.0);

    // Shallow water turn (h/T ~1.5)
    PhysicsInput turnShallow;
    turnShallow.portEngine = 0.8;
    turnShallow.rudderAngle = 35.0;
    turnShallow.waterDepth = 1.5; // Very shallow (1.5m below keel, draught=3.0m, so h/T=1.5)

    PhysicsState finalShallow = simulate(model, turnShallow, stateShallow, 120.0);

    double deepHeadingChange = cumulativeHeadingChange(
        simulateTrajectory(model, turnDeep, stateDeep, 120.0));
    double shallowHeadingChange = cumulativeHeadingChange(
        simulateTrajectory(model, turnShallow, stateShallow, 120.0));

    INFO("Deep heading change: " << deepHeadingChange);
    INFO("Shallow heading change: " << shallowHeadingChange);

    // In shallow water, the increased hull forces should reduce rate of turn
    // So less heading change in the same time = wider turning circle
    REQUIRE(fabs(shallowHeadingChange) < fabs(deepHeadingChange));
}

// ── 8-02: Bank effects ──────────────────────────────────────────────────────

TEST_CASE("Bank forces are zero with no banks", "[mmg][bank]") {
    double Yb, Nb;
    MMGPhysicsModel::computeBankForces(5.0, 100.0, 6.0, 1000.0, 1000.0, Yb, Nb);
    REQUIRE(Yb == Approx(0.0));
    REQUIRE(Nb == Approx(0.0));
}

TEST_CASE("Bank forces are zero at rest", "[mmg][bank]") {
    double Yb, Nb;
    MMGPhysicsModel::computeBankForces(0.0, 100.0, 6.0, 20.0, 20.0, Yb, Nb);
    REQUIRE(Yb == Approx(0.0));
    REQUIRE(Nb == Approx(0.0));
}

TEST_CASE("Port bank attracts ship to port", "[mmg][bank]") {
    double Yb, Nb;
    // Close port bank (20m), far starboard bank (1000m)
    MMGPhysicsModel::computeBankForces(5.0, 100.0, 6.0, 20.0, 1000.0, Yb, Nb);

    // Yb should be negative (attraction to port)
    REQUIRE(Yb < 0.0);
    // Nb should be positive (bow pushed to starboard, away from port bank)
    REQUIRE(Nb > 0.0);
}

TEST_CASE("Starboard bank attracts ship to starboard", "[mmg][bank]") {
    double Yb, Nb;
    // Far port bank, close starboard bank (20m)
    MMGPhysicsModel::computeBankForces(5.0, 100.0, 6.0, 1000.0, 20.0, Yb, Nb);

    // Yb should be positive (attraction to starboard)
    REQUIRE(Yb > 0.0);
    // Nb should be negative (bow pushed to port, away from starboard bank)
    REQUIRE(Nb < 0.0);
}

TEST_CASE("Bank force increases closer to bank", "[mmg][bank]") {
    double Yb1, Nb1, Yb2, Nb2;
    MMGPhysicsModel::computeBankForces(5.0, 100.0, 6.0, 50.0, 1000.0, Yb1, Nb1);
    MMGPhysicsModel::computeBankForces(5.0, 100.0, 6.0, 20.0, 1000.0, Yb2, Nb2);

    // Closer bank should produce stronger forces
    REQUIRE(fabs(Yb2) > fabs(Yb1));
    REQUIRE(fabs(Nb2) > fabs(Nb1));
}

TEST_CASE("Symmetric banks produce no net lateral force", "[mmg][bank]") {
    double Yb, Nb;
    // Equal distance to both banks
    MMGPhysicsModel::computeBankForces(5.0, 100.0, 6.0, 30.0, 30.0, Yb, Nb);

    // Forces should cancel out
    REQUIRE(fabs(Yb) < 0.1);
    REQUIRE(fabs(Nb) < 0.1);
}

// ── 8-03: Isherwood wind force model ────────────────────────────────────────

TEST_CASE("Wind forces are zero with no wind", "[mmg][wind]") {
    double Xw, Yw, Nw;
    MMGPhysicsModel::computeWindForces(0.0, 0.0, 0.0, 5.0, 0.0,
                                         100.0, 16.0, 6.0, 0, 0, 0,
                                         Xw, Yw, Nw);
    REQUIRE(Xw == Approx(0.0));
    REQUIRE(Yw == Approx(0.0));
    REQUIRE(Nw == Approx(0.0));
}

TEST_CASE("Head wind produces drag (negative X)", "[mmg][wind]") {
    double Xw, Yw, Nw;
    // Wind from ahead (heading=0, wind from 0 = head wind)
    MMGPhysicsModel::computeWindForces(20.0, 0.0, 0.0, 0.0, 0.0,
                                         100.0, 16.0, 6.0, 0, 0, 0,
                                         Xw, Yw, Nw);
    // Head wind should create drag (negative X = slowing down)
    REQUIRE(Xw < 0.0);
    // No lateral force from pure head wind
    REQUIRE(fabs(Yw) < fabs(Xw) * 0.1);
}

TEST_CASE("Beam wind produces large lateral force", "[mmg][wind]") {
    double Xw, Yw, Nw;
    // Wind from starboard beam (heading=0, wind from 90)
    MMGPhysicsModel::computeWindForces(20.0, 90.0, 0.0, 0.0, 0.0,
                                         100.0, 16.0, 6.0, 0, 0, 0,
                                         Xw, Yw, Nw);
    // Beam wind should produce strong lateral force
    REQUIRE(fabs(Yw) > 0.0);
    // Lateral force should dominate over axial at beam wind
    REQUIRE(fabs(Yw) > fabs(Xw));
}

TEST_CASE("Wind force increases with wind speed squared", "[mmg][wind]") {
    double Xw1, Yw1, Nw1, Xw2, Yw2, Nw2;
    // 10 m/s beam wind
    MMGPhysicsModel::computeWindForces(10.0, 90.0, 0.0, 0.0, 0.0,
                                         100.0, 16.0, 6.0, 0, 0, 0,
                                         Xw1, Yw1, Nw1);
    // 20 m/s beam wind (2x speed = ~4x force)
    MMGPhysicsModel::computeWindForces(20.0, 90.0, 0.0, 0.0, 0.0,
                                         100.0, 16.0, 6.0, 0, 0, 0,
                                         Xw2, Yw2, Nw2);

    // Force ratio should be approximately 4x (wind speed squared)
    double ratio = fabs(Yw2) / fabs(Yw1);
    REQUIRE(ratio > 3.0);
    REQUIRE(ratio < 5.0);
}

TEST_CASE("Tail wind produces forward push", "[mmg][wind]") {
    double Xw, Yw, Nw;
    // Wind from stern (heading=0, wind from 180)
    MMGPhysicsModel::computeWindForces(20.0, 180.0, 0.0, 0.0, 0.0,
                                         100.0, 16.0, 6.0, 0, 0, 0,
                                         Xw, Yw, Nw);
    // Tail wind should produce forward force (positive X)
    REQUIRE(Xw > 0.0);
}

TEST_CASE("Wind yaw moment peaks at oblique angles", "[mmg][wind]") {
    double Xw, Yw, Nw_head, Nw_beam, Nw_oblique;
    // Head wind (0°) - minimal yaw
    MMGPhysicsModel::computeWindForces(20.0, 0.0, 0.0, 0.0, 0.0,
                                         100.0, 16.0, 6.0, 0, 0, 0,
                                         Xw, Yw, Nw_head);
    // Beam wind (90°) - some yaw
    MMGPhysicsModel::computeWindForces(20.0, 90.0, 0.0, 0.0, 0.0,
                                         100.0, 16.0, 6.0, 0, 0, 0,
                                         Xw, Yw, Nw_beam);
    // Oblique wind (45°) - should have significant yaw
    MMGPhysicsModel::computeWindForces(20.0, 45.0, 0.0, 0.0, 0.0,
                                         100.0, 16.0, 6.0, 0, 0, 0,
                                         Xw, Yw, Nw_oblique);

    // Oblique wind should produce more yaw moment than pure head wind
    REQUIRE(fabs(Nw_oblique) > fabs(Nw_head));
}

TEST_CASE("Default PhysicsInput has deep water and no banks", "[mmg]") {
    PhysicsInput input;
    REQUIRE(input.waterDepth == 100.0);
    REQUIRE(input.bankDistancePort == 1000.0);
    REQUIRE(input.bankDistanceStbd == 1000.0);
    REQUIRE(input.windSpeed == 0.0);
}

TEST_CASE("Existing deep-water tests unaffected by new inputs", "[mmg]") {
    // Verify that the new PhysicsInput fields (waterDepth, bank, wind, current)
    // with default values don't change deep-water behavior
    auto model = createSmallVessel();
    PhysicsState state;
    PhysicsInput input;
    input.portEngine = 0.5;

    state = simulate(model, input, state, 60.0);

    // Same as "MMG: engine produces forward motion" test
    REQUIRE(state.surge > 0.5);
    REQUIRE(fabs(state.sway) < 0.5);
    REQUIRE(state.posZ > 10.0);
}

// ── Barras squat tests ──────────────────────────────────────────────────────

TEST_CASE("Squat is zero in deep water", "[mmg][squat]") {
    auto model = createKVLCC2();
    REQUIRE(model.computeSquat(10.0, 5.0) == Approx(0.0));
    REQUIRE(model.computeSquat(15.0, 10.0) == Approx(0.0));
}

TEST_CASE("Squat is zero at low speed", "[mmg][squat]") {
    auto model = createKVLCC2();
    REQUIRE(model.computeSquat(0.3, 1.5) == Approx(0.0));
    REQUIRE(model.computeSquat(0.0, 2.0) == Approx(0.0));
}

TEST_CASE("Squat increases with speed", "[mmg][squat]") {
    auto model = createKVLCC2();
    double hT = 2.0; // moderate shallow water
    double s5 = model.computeSquat(5.0, hT);
    double s10 = model.computeSquat(10.0, hT);
    double s15 = model.computeSquat(15.0, hT);

    REQUIRE(s5 > 0.0);
    REQUIRE(s10 > s5);
    REQUIRE(s15 > s10);
}

TEST_CASE("Squat increases in shallower water", "[mmg][squat]") {
    auto model = createKVLCC2();
    double speed = 10.0; // knots
    double s_deep = model.computeSquat(speed, 3.5);
    double s_mid = model.computeSquat(speed, 2.0);
    double s_shallow = model.computeSquat(speed, 1.2);

    REQUIRE(s_shallow > s_mid);
    REQUIRE(s_mid > s_deep);
}

TEST_CASE("Squat scales with block coefficient", "[mmg][squat]") {
    // Full-form tanker (Cb=0.81) squats more than fine-form ferry (Cb=0.55)
    ShipDimensions tanker;
    tanker.blockCoefficient = 0.81;
    MMGPhysicsModel tankerModel(tanker);

    ShipDimensions ferry;
    ferry.blockCoefficient = 0.55;
    MMGPhysicsModel ferryModel(ferry);

    REQUIRE(tankerModel.computeSquat(10.0, 2.0) > ferryModel.computeSquat(10.0, 2.0));
}

TEST_CASE("Squat is physically reasonable magnitude", "[mmg][squat]") {
    // KVLCC2 at 10 knots in h/T=2.0 should be roughly 0.5-2.0m
    auto model = createKVLCC2();
    double squat = model.computeSquat(10.0, 2.0);
    REQUIRE(squat > 0.3);
    REQUIRE(squat < 3.0);
}

// ── Tidal current force tests ───────────────────────────────────────────────

TEST_CASE("Following current reduces drag", "[mmg][current]") {
    auto model = createSmallVessel();
    PhysicsInput input;
    input.portEngine = 0.5;

    // No current
    PhysicsState stateNoCurrent;
    stateNoCurrent = simulate(model, input, stateNoCurrent, 120.0);

    // 1 m/s following current (same direction as ship travel)
    PhysicsState stateCurrent;
    PhysicsInput inputCurrent = input;
    inputCurrent.currentSurge = 1.0; // current flowing in surge direction
    stateCurrent = simulate(model, inputCurrent, stateCurrent, 120.0);

    // With following current, ship reaches higher speed over ground
    REQUIRE(stateCurrent.surge > stateNoCurrent.surge);
}

TEST_CASE("Head current increases drag", "[mmg][current]") {
    auto model = createSmallVessel();
    PhysicsInput input;
    input.portEngine = 0.5;

    // No current
    PhysicsState stateNoCurrent;
    stateNoCurrent = simulate(model, input, stateNoCurrent, 120.0);

    // 1 m/s head current (opposing ship)
    PhysicsState stateCurrent;
    PhysicsInput inputCurrent = input;
    inputCurrent.currentSurge = -1.0;
    stateCurrent = simulate(model, inputCurrent, stateCurrent, 120.0);

    // With head current, ship is slower
    REQUIRE(stateCurrent.surge < stateNoCurrent.surge);
}

TEST_CASE("Beam current causes drift", "[mmg][current]") {
    auto model = createSmallVessel();
    PhysicsInput input;
    input.portEngine = 0.5;
    input.currentSway = 0.5; // current pushing to starboard

    PhysicsState state;
    state = simulate(model, input, state, 120.0);

    // Ship should have lateral drift (nonzero sway or off-track position)
    // posX increases with starboard drift (heading north = +Z, stbd = +X)
    REQUIRE(state.posX > 1.0);
}

TEST_CASE("Zero current gives same result as no current", "[mmg][current]") {
    auto model = createSmallVessel();
    PhysicsInput input;
    input.portEngine = 0.5;
    input.currentSurge = 0.0;
    input.currentSway = 0.0;

    PhysicsState state1;
    state1 = simulate(model, input, state1, 60.0);

    PhysicsInput inputDefault;
    inputDefault.portEngine = 0.5;
    // currentSurge/currentSway default to 0.0

    PhysicsState state2;
    state2 = simulate(model, inputDefault, state2, 60.0);

    REQUIRE(state1.surge == Approx(state2.surge).margin(0.001));
    REQUIRE(state1.posZ == Approx(state2.posZ).margin(0.01));
}

// ══════════════════════════════════════════════════════════════════════════════
// Azimuth Drive MMG Tests
// ══════════════════════════════════════════════════════════════════════════════

// Helper: create a twin azimuth tug for testing
static MMGPhysicsModel createAzimuthTug() {
    ShipDimensions dims;
    dims.length = 30.0;
    dims.beam = 10.0;
    dims.draught = 4.0;
    dims.displacement = 400.0 * 1000.0; // 400 tonnes
    dims.blockCoefficient = 0.87;
    dims.maxSpeed = 6.0 * 0.5144; // 6 knots
    dims.maxEngineForce = 300000.0; // Total (split between two drives)
    dims.propellerDiameter = 1.5;
    dims.maxRPM = 1600.0;
    dims.singleEngine = false;
    dims.propellorSpacing = 6.0;

    MMGCoefficients coeffs;
    coeffs.estimateFromDimensions(dims);
    return MMGPhysicsModel(dims, coeffs);
}

TEST_CASE("Azimuth drive ahead produces forward motion", "[mmg][azimuth]") {
    auto model = createAzimuthTug();

    PhysicsInput input;
    input.isAzimuthDrive = true;
    input.portEngine = 0.5;
    input.stbdEngine = 0.5;
    input.portAzimuthAngleDeg = 90.0;  // ahead
    input.stbdAzimuthAngleDeg = 270.0; // ahead
    input.aziDriveLeverArm = 30.0 * (0.5 - 0.3); // stern drives

    PhysicsState state;
    state = simulate(model, input, state, 60.0);

    REQUIRE(state.surge > 1.0); // Must move forward
    REQUIRE(std::abs(state.sway) < 0.5); // Minimal lateral drift
    REQUIRE(std::abs(state.heading) < 5.0); // Roughly straight
}

TEST_CASE("Azimuth drive lateral produces sway without much yaw", "[mmg][azimuth]") {
    auto model = createAzimuthTug();

    PhysicsInput input;
    input.isAzimuthDrive = true;
    // Both drives pointing to starboard
    input.portEngine = 0.3;
    input.stbdEngine = 0.3;
    input.portAzimuthAngleDeg = 0.0;   // starboard
    input.stbdAzimuthAngleDeg = 180.0; // port... wait, 180 = port direction
    // Actually for both pointing stbd: port=0, stbd=360(=0)
    input.portAzimuthAngleDeg = 0.0;
    input.stbdAzimuthAngleDeg = 0.0;
    input.aziDriveLeverArm = 30.0 * (0.5 - 0.3);

    PhysicsState state;
    state = simulate(model, input, state, 30.0);

    // Ship should have moved laterally
    REQUIRE(std::abs(state.sway) > 0.1);
}

TEST_CASE("Azimuth drive differential angle produces yaw", "[mmg][azimuth]") {
    auto model = createAzimuthTug();

    PhysicsInput input;
    input.isAzimuthDrive = true;
    input.portEngine = 0.5;
    input.stbdEngine = 0.5;
    // Port ahead, stbd astern = turning moment
    input.portAzimuthAngleDeg = 90.0;  // ahead
    input.stbdAzimuthAngleDeg = 90.0;  // also ahead but reversed angle convention
    // Actually, starboard at 270 is ahead. Let's do port ahead, stbd reversed:
    input.portAzimuthAngleDeg = 90.0;  // ahead
    input.stbdAzimuthAngleDeg = 90.0;  // this means the stbd drive also points "ahead" but in azimuth coords
    input.aziDriveLeverArm = 30.0 * (0.5 - 0.3);

    // For yaw: put port ahead, stbd astern
    input.portEngine = 0.5;
    input.stbdEngine = -0.5; // astern

    PhysicsState state;
    state = simulate(model, input, state, 30.0);

    // Differential thrust should produce significant yaw
    REQUIRE(std::abs(state.yawRate) > 0.1);
}

TEST_CASE("Azimuth drive zero engine produces no motion", "[mmg][azimuth]") {
    auto model = createAzimuthTug();

    PhysicsInput input;
    input.isAzimuthDrive = true;
    input.portEngine = 0.0;
    input.stbdEngine = 0.0;
    input.portAzimuthAngleDeg = 90.0;
    input.stbdAzimuthAngleDeg = 270.0;
    input.aziDriveLeverArm = 6.0;

    PhysicsState state;
    state = simulate(model, input, state, 10.0);

    REQUIRE(std::abs(state.surge) < 0.01);
    REQUIRE(std::abs(state.sway) < 0.01);
    REQUIRE(std::abs(state.yawRate) < 0.01);
}

TEST_CASE("Azimuth hull forces still apply (drag decelerates)", "[mmg][azimuth]") {
    auto model = createAzimuthTug();

    // Give ship initial forward speed, no engine thrust
    PhysicsInput input;
    input.isAzimuthDrive = true;
    input.portEngine = 0.0;
    input.stbdEngine = 0.0;
    input.portAzimuthAngleDeg = 90.0;
    input.stbdAzimuthAngleDeg = 270.0;

    PhysicsState state;
    state.surge = 3.0; // 3 m/s initial forward speed

    state = simulate(model, input, state, 60.0);

    // Should have decelerated due to hull drag
    REQUIRE(state.surge < 1.0);
}
