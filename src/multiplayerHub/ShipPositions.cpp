/*   Bridge Command 5.0 Ship Simulator
     Copyright (C) 2016 James Packer

     This program is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License version 2 as
     published by the Free Software Foundation

     This program is distributed in the hope that it will be useful,
     but WITHOUT ANY WARRANTY; without even the implied warranty of
     MERCHANTABILITY Or FITNESS For A PARTICULAR PURPOSE.  See the
     GNU General Public License For more details.

     You should have received a copy of the GNU General Public License along
     with this program; if not, write to the Free Software Foundation, Inc.,
     51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA. */

#include "ShipPositions.hpp"
#include "../Constants.hpp"

ShipPositions::ShipPositions(unsigned int numberOfShips)
{
    for (unsigned int i = 0; i < numberOfShips; i++) {
        shipData.push_back(ShipData());
    }
}

void ShipPositions::addShip()
{
    shipData.push_back(ShipData());
}

unsigned int ShipPositions::getNumberOfShips() const
{
    return (unsigned int)shipData.size();
}

void ShipPositions::extrapolateArc(const ShipState& state, float time, float& x, float& z, float& bearing)
{
    float dt = time - state.time;
    if (dt < 0) dt = 0;

    // Arc extrapolation: use average bearing over the time step
    bearing = state.bearing + state.rateOfTurn * dt;
    float avgBearing = state.bearing + state.rateOfTurn * dt / 2.0f;
    float distance = state.speed * dt;

    x = state.x + distance * sin(avgBearing * RAD_IN_DEG);
    z = state.z + distance * cos(avgBearing * RAD_IN_DEG);
}

void ShipPositions::setShipPosition(unsigned int shipNumber, float scenarioTime, float positionX, float positionZ, float speed, float bearing, float rateOfTurn)
{
    if (shipNumber < shipData.size()) {
        ShipData& ship = shipData[shipNumber];

        // Calculate where we expected the ship to be (dead-reckoned from previous state)
        if (ship.current.time > 0) {
            float expectedX, expectedZ, expectedBrg;
            extrapolateArc(ship.current, scenarioTime, expectedX, expectedZ, expectedBrg);

            float dx = expectedX - positionX;
            float dz = expectedZ - positionZ;
            float correctionDist = sqrt(dx * dx + dz * dz);

            if (correctionDist > 0.1f && correctionDist < SNAP_THRESHOLD) {
                // Small correction: blend out the error over BLEND_DURATION
                ship.blending = true;
                ship.blendStartTime = scenarioTime;
                ship.errorX = dx;
                ship.errorZ = dz;
                ship.errorBearing = expectedBrg - bearing;
            } else {
                // Negligible or large correction: snap immediately
                ship.blending = false;
            }
        }

        // Store the new state
        ship.current.time = scenarioTime;
        ship.current.x = positionX;
        ship.current.z = positionZ;
        ship.current.speed = speed;
        ship.current.bearing = bearing;
        ship.current.rateOfTurn = rateOfTurn;
    }
}

void ShipPositions::getShipPosition(const unsigned int& shipNumber, const float& scenarioTime, float& positionX, float& positionZ, float& speed, float& bearing, float& rateOfTurn)
{
    if (shipNumber < shipData.size()) {
        ShipData& ship = shipData[shipNumber];

        speed = ship.current.speed;
        rateOfTurn = ship.current.rateOfTurn;

        // Dead-reckon from current state using arc extrapolation
        float drX, drZ, drBrg;
        extrapolateArc(ship.current, scenarioTime, drX, drZ, drBrg);

        if (ship.blending) {
            float elapsed = scenarioTime - ship.blendStartTime;
            float t = elapsed / BLEND_DURATION;

            if (t >= 1.0f) {
                // Blend complete
                ship.blending = false;
                positionX = drX;
                positionZ = drZ;
                bearing = drBrg;
            } else {
                // Smoothstep: t^2 * (3 - 2t)
                t = t * t * (3.0f - 2.0f * t);

                // Apply diminishing error offset to the dead-reckoned position
                positionX = drX + (1.0f - t) * ship.errorX;
                positionZ = drZ + (1.0f - t) * ship.errorZ;
                bearing = drBrg + (1.0f - t) * ship.errorBearing;
            }
        } else {
            positionX = drX;
            positionZ = drZ;
            bearing = drBrg;
        }

    } else {
        speed = 0;
        positionX = 0;
        positionZ = 0;
        bearing = 0;
        rateOfTurn = 0;
    }
}

void ShipPositions::setExtendedState(unsigned int shipNumber, float rudderAngle, float engineRPM, int navLights, int hornActive, uint32_t mmsi)
{
    if (shipNumber < shipData.size()) {
        shipData[shipNumber].current.rudderAngle = rudderAngle;
        shipData[shipNumber].current.engineRPM = engineRPM;
        shipData[shipNumber].current.navLights = navLights;
        shipData[shipNumber].current.hornActive = hornActive;
        shipData[shipNumber].current.mmsi = mmsi;
    }
}

void ShipPositions::getExtendedState(unsigned int shipNumber, float& rudderAngle, float& engineRPM, int& navLights, int& hornActive, uint32_t& mmsi) const
{
    if (shipNumber < shipData.size()) {
        rudderAngle = shipData[shipNumber].current.rudderAngle;
        engineRPM = shipData[shipNumber].current.engineRPM;
        navLights = shipData[shipNumber].current.navLights;
        hornActive = shipData[shipNumber].current.hornActive;
        mmsi = shipData[shipNumber].current.mmsi;
    } else {
        rudderAngle = 0;
        engineRPM = 0;
        navLights = 0;
        hornActive = 0;
        mmsi = 0;
    }
}
