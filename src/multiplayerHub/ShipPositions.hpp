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

#ifndef __SHIPPOSITIONS_HPP_INCLUDED__
#define __SHIPPOSITIONS_HPP_INCLUDED__

#include <vector>
#include <cmath>
#include <cstdint>

struct ShipState {
    float time;
    float x, z;
    float speed, bearing, rateOfTurn; // speed in m/s, bearing in deg, rateOfTurn in deg/s
    // Extended state
    float rudderAngle;
    float engineRPM;
    int navLights;
    int hornActive;
    uint32_t mmsi;
    ShipState() : time(0), x(0), z(0), speed(0), bearing(0), rateOfTurn(0),
                  rudderAngle(0), engineRPM(0), navLights(0), hornActive(0), mmsi(0) {}
};

struct ShipData {
    ShipState current;          // Most recent received update

    // Error blending state
    bool blending;
    float blendStartTime;
    float errorX, errorZ;       // Positional error to blend out
    float errorBearing;         // Heading error to blend out

    ShipData() : blending(false), blendStartTime(0), errorX(0), errorZ(0), errorBearing(0) {}
};

//hold current positions, headings and speeds of other ships

class ShipPositions {

    public:
    ShipPositions(unsigned int numberOfShips);

    void addShip(); // Add a new ship slot (for late join)
    unsigned int getNumberOfShips() const;

    void setShipPosition(unsigned int shipNumber, float scenarioTime, float positionX, float positionZ, float speed, float bearing, float rateOfTurn);
    void getShipPosition(const unsigned int& shipNumber, const float& scenarioTime, float& positionX, float& positionZ, float& speed, float& bearing, float& rateOfTurn);

    void setExtendedState(unsigned int shipNumber, float rudderAngle, float engineRPM, int navLights, int hornActive, uint32_t mmsi);
    void getExtendedState(unsigned int shipNumber, float& rudderAngle, float& engineRPM, int& navLights, int& hornActive, uint32_t& mmsi) const;

    private:
    std::vector<ShipData> shipData;

    // Arc extrapolation from a state to a future time
    static void extrapolateArc(const ShipState& state, float time, float& x, float& z, float& bearing);

    static constexpr float BLEND_DURATION = 0.5f;  // Blend corrections over 500ms
    static constexpr float SNAP_THRESHOLD = 5.0f;   // Snap if correction > 5m
};


#endif // __SHIPPOSITIONS_HPP_INCLUDED__
