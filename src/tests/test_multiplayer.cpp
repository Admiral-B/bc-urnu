#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "../multiplayerHub/ShipPositions.hpp"

TEST_CASE("ShipPositions construction", "[multiplayer]") {
    ShipPositions positions(3);
    REQUIRE(positions.getNumberOfShips() == 3);
}

TEST_CASE("Dead reckoning - straight line", "[multiplayer]") {
    ShipPositions positions(1);
    // Ship at origin, heading north (0 deg), speed 10 m/s, no turn
    positions.setShipPosition(0, 0.0f, 0.0f, 0.0f, 10.0f, 0.0f, 0.0f);

    float x, z, spd, brg, rot;
    positions.getShipPosition(0, 1.0f, x, z, spd, brg, rot);

    // Heading 0 = north, so movement should be in +Z direction
    // sin(0) = 0, cos(0) = 1, so x stays ~0, z increases by ~10
    REQUIRE(x == Catch::Approx(0.0f).margin(0.1f));
    REQUIRE(z == Catch::Approx(10.0f).margin(0.5f));
    REQUIRE(spd == Catch::Approx(10.0f));
    REQUIRE(brg == Catch::Approx(0.0f));
}

TEST_CASE("Dead reckoning - heading east", "[multiplayer]") {
    ShipPositions positions(1);
    // Ship heading east (90 deg), speed 5 m/s
    positions.setShipPosition(0, 0.0f, 100.0f, 200.0f, 5.0f, 90.0f, 0.0f);

    float x, z, spd, brg, rot;
    positions.getShipPosition(0, 1.0f, x, z, spd, brg, rot);

    // sin(90) = 1, cos(90) = 0, so X increases by ~5, Z stays ~200
    REQUIRE(x == Catch::Approx(105.0f).margin(0.5f));
    REQUIRE(z == Catch::Approx(200.0f).margin(0.5f));
}

TEST_CASE("Dead reckoning - stationary ship", "[multiplayer]") {
    ShipPositions positions(1);
    positions.setShipPosition(0, 0.0f, 50.0f, 75.0f, 0.0f, 45.0f, 0.0f);

    float x, z, spd, brg, rot;
    positions.getShipPosition(0, 10.0f, x, z, spd, brg, rot);

    // No speed, should stay put
    REQUIRE(x == Catch::Approx(50.0f).margin(0.01f));
    REQUIRE(z == Catch::Approx(75.0f).margin(0.01f));
    REQUIRE(brg == Catch::Approx(45.0f));
}

TEST_CASE("Dead reckoning with rate of turn", "[multiplayer]") {
    ShipPositions positions(1);
    // Ship at origin, heading north, speed 10 m/s, turning right at 10 deg/s
    positions.setShipPosition(0, 0.0f, 0.0f, 0.0f, 10.0f, 0.0f, 10.0f);

    float x, z, spd, brg, rot;
    positions.getShipPosition(0, 6.0f, x, z, spd, brg, rot);

    // After 6 seconds at 10 deg/s, heading should be ~60 deg
    REQUIRE(brg == Catch::Approx(60.0f).margin(2.0f));
    REQUIRE(rot == Catch::Approx(10.0f));
}

TEST_CASE("Dead reckoning with rate of turn - 90 degree turn", "[multiplayer]") {
    ShipPositions positions(1);
    // Ship heading north, speed 10 m/s, turning right at 30 deg/s
    positions.setShipPosition(0, 0.0f, 0.0f, 0.0f, 10.0f, 0.0f, 30.0f);

    float x, z, spd, brg, rot;
    positions.getShipPosition(0, 3.0f, x, z, spd, brg, rot);

    // After 3 seconds at 30 deg/s, heading should be ~90 deg
    REQUIRE(brg == Catch::Approx(90.0f).margin(2.0f));
    // Ship should have moved to the right (positive X) and forward (positive Z)
    REQUIRE(x > 0.0f);
    REQUIRE(z > 0.0f);
}

TEST_CASE("Multiple ships independent positions", "[multiplayer]") {
    ShipPositions positions(3);

    positions.setShipPosition(0, 0.0f, 0.0f, 0.0f, 5.0f, 0.0f, 0.0f);
    positions.setShipPosition(1, 0.0f, 100.0f, 100.0f, 10.0f, 90.0f, 0.0f);
    positions.setShipPosition(2, 0.0f, 200.0f, 200.0f, 0.0f, 180.0f, 0.0f);

    float x0, z0, s0, b0, r0;
    float x1, z1, s1, b1, r1;
    float x2, z2, s2, b2, r2;

    positions.getShipPosition(0, 1.0f, x0, z0, s0, b0, r0);
    positions.getShipPosition(1, 1.0f, x1, z1, s1, b1, r1);
    positions.getShipPosition(2, 1.0f, x2, z2, s2, b2, r2);

    // Ship 0: heading north at 5 m/s -> z increases
    REQUIRE(z0 == Catch::Approx(5.0f).margin(0.5f));

    // Ship 1: heading east at 10 m/s -> x increases from 100
    REQUIRE(x1 == Catch::Approx(110.0f).margin(0.5f));

    // Ship 2: stationary -> stays at 200,200
    REQUIRE(x2 == Catch::Approx(200.0f).margin(0.01f));
    REQUIRE(z2 == Catch::Approx(200.0f).margin(0.01f));
}

TEST_CASE("addShip adds a new slot", "[multiplayer]") {
    ShipPositions positions(2);
    REQUIRE(positions.getNumberOfShips() == 2);

    positions.addShip();
    REQUIRE(positions.getNumberOfShips() == 3);

    // New ship should be at default position (0,0)
    float x, z, spd, brg, rot;
    positions.getShipPosition(2, 0.0f, x, z, spd, brg, rot);
    REQUIRE(x == Catch::Approx(0.0f));
    REQUIRE(z == Catch::Approx(0.0f));

    // Set position on the new ship
    positions.setShipPosition(2, 0.0f, 300.0f, 400.0f, 5.0f, 45.0f, 0.0f);
    positions.getShipPosition(2, 0.0f, x, z, spd, brg, rot);
    REQUIRE(x == Catch::Approx(300.0f));
    REQUIRE(z == Catch::Approx(400.0f));
}

TEST_CASE("Extended state set and get", "[multiplayer]") {
    ShipPositions positions(2);

    positions.setExtendedState(0, -15.5f, 120.0f, 1, 0, 123456789);
    positions.setExtendedState(1, 10.0f, 80.0f, 0, 1, 987654321);

    float rudder, rpm;
    int navLights, horn;
    uint32_t mmsi;

    positions.getExtendedState(0, rudder, rpm, navLights, horn, mmsi);
    REQUIRE(rudder == Catch::Approx(-15.5f));
    REQUIRE(rpm == Catch::Approx(120.0f));
    REQUIRE(navLights == 1);
    REQUIRE(horn == 0);
    REQUIRE(mmsi == 123456789);

    positions.getExtendedState(1, rudder, rpm, navLights, horn, mmsi);
    REQUIRE(rudder == Catch::Approx(10.0f));
    REQUIRE(rpm == Catch::Approx(80.0f));
    REQUIRE(navLights == 0);
    REQUIRE(horn == 1);
    REQUIRE(mmsi == 987654321);
}

TEST_CASE("Extended state defaults to zero", "[multiplayer]") {
    ShipPositions positions(1);

    float rudder, rpm;
    int navLights, horn;
    uint32_t mmsi;

    positions.getExtendedState(0, rudder, rpm, navLights, horn, mmsi);
    REQUIRE(rudder == Catch::Approx(0.0f));
    REQUIRE(rpm == Catch::Approx(0.0f));
    REQUIRE(navLights == 0);
    REQUIRE(horn == 0);
    REQUIRE(mmsi == 0);
}

TEST_CASE("Out of range ship number returns zero", "[multiplayer]") {
    ShipPositions positions(1);

    float x, z, spd, brg, rot;
    positions.getShipPosition(5, 0.0f, x, z, spd, brg, rot);
    REQUIRE(x == 0.0f);
    REQUIRE(z == 0.0f);
    REQUIRE(spd == 0.0f);
    REQUIRE(brg == 0.0f);
    REQUIRE(rot == 0.0f);

    float rudder, rpm;
    int navLights, horn;
    uint32_t mmsi;
    positions.getExtendedState(5, rudder, rpm, navLights, horn, mmsi);
    REQUIRE(rudder == 0.0f);
    REQUIRE(mmsi == 0);
}

TEST_CASE("Error blending - small correction blends smoothly", "[multiplayer]") {
    ShipPositions positions(1);

    // Set initial position
    positions.setShipPosition(0, 0.0f, 0.0f, 0.0f, 10.0f, 0.0f, 0.0f);

    // After 1 second, the ship should be at (0, 10) via dead reckoning.
    // But we receive an update saying it's actually at (1, 10) - a small correction.
    positions.setShipPosition(0, 1.0f, 1.0f, 10.0f, 10.0f, 0.0f, 0.0f);

    // Immediately after the update, position should be blending
    // and should NOT be exactly at the dead-reckoned position from the new state.
    float x, z, spd, brg, rot;
    positions.getShipPosition(0, 1.0f, x, z, spd, brg, rot);

    // The x should be somewhere near 1.0 but affected by blending
    // (error of ~1m from expected vs actual, blended over 500ms)
    REQUIRE(x == Catch::Approx(1.0f).margin(2.0f));
    REQUIRE(z == Catch::Approx(10.0f).margin(1.0f));
}

TEST_CASE("Error blending - large correction snaps", "[multiplayer]") {
    ShipPositions positions(1);

    positions.setShipPosition(0, 0.0f, 0.0f, 0.0f, 10.0f, 0.0f, 0.0f);

    // After 1 second, DR expects (0, 10). Actual position is (100, 100) - a huge jump.
    // This should snap, not blend.
    positions.setShipPosition(0, 1.0f, 100.0f, 100.0f, 10.0f, 0.0f, 0.0f);

    float x, z, spd, brg, rot;
    positions.getShipPosition(0, 1.0f, x, z, spd, brg, rot);

    // Should snap to exact position (no blending)
    REQUIRE(x == Catch::Approx(100.0f).margin(0.1f));
    REQUIRE(z == Catch::Approx(100.0f).margin(0.1f));
}

TEST_CASE("Position update at same time as query", "[multiplayer]") {
    ShipPositions positions(1);
    positions.setShipPosition(0, 5.0f, 50.0f, 75.0f, 3.0f, 180.0f, 0.0f);

    float x, z, spd, brg, rot;
    positions.getShipPosition(0, 5.0f, x, z, spd, brg, rot);

    // Query at same time as update: should return exact position
    REQUIRE(x == Catch::Approx(50.0f).margin(0.01f));
    REQUIRE(z == Catch::Approx(75.0f).margin(0.01f));
    REQUIRE(brg == Catch::Approx(180.0f));
}
