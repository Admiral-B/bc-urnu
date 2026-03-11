/*   Bridge Command 5.0 Ship Simulator
     Copyright (C) 2024 James Packer

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

#ifndef __PHYSICSMODEL_HPP_INCLUDED__
#define __PHYSICSMODEL_HPP_INCLUDED__

#include <cmath>

// State vector for 3-DOF ship motion (surge, sway, yaw)
struct PhysicsState {
    double posX = 0.0;       // Position X (internal coords, metres)
    double posZ = 0.0;       // Position Z (internal coords, metres)
    double heading = 0.0;    // Heading (degrees, 0=North, CW positive)
    double surge = 0.0;      // Forward velocity (m/s, body frame)
    double sway = 0.0;       // Lateral velocity (m/s, body frame, +ve = starboard)
    double yawRate = 0.0;    // Yaw angular velocity (deg/s, CW positive)
    double roll = 0.0;       // Roll angle (degrees)
    double pitch = 0.0;      // Pitch angle (degrees)
};

// Control and environment inputs
struct PhysicsInput {
    double rudderAngle = 0.0;    // Rudder angle (degrees, +ve = starboard)
    double portEngine = 0.0;     // Port engine setting (-1 to +1)
    double stbdEngine = 0.0;     // Starboard engine setting (-1 to +1)
    double windSpeed = 0.0;      // True wind speed (m/s)
    double windDirection = 0.0;  // True wind direction (degrees, from)
    double currentSpeed = 0.0;   // Water current speed (m/s) [unused, use body-frame below]
    double currentDirection = 0.0; // Water current direction (degrees, towards) [unused]
    double currentSurge = 0.0;   // Current in surge direction (m/s, body frame, +ve = ahead)
    double currentSway = 0.0;    // Current in sway direction (m/s, body frame, +ve = starboard)
    double waveHeight = 0.0;     // Significant wave height (m)
    double waterDepth = 100.0;   // Water depth below keel (m, 100 = deep water default)
    double bankDistancePort = 1000.0;  // Distance to bank on port side (m, 1000 = no effect)
    double bankDistanceStbd = 1000.0;  // Distance to bank on starboard side (m, 1000 = no effect)

    // Azimuth drive parameters (ignored when isAzimuthDrive is false)
    bool isAzimuthDrive = false;
    double portAzimuthAngleDeg = 90.0;  // Port azimuth angle (degrees, 90=ahead, 0=stbd, 180=port)
    double stbdAzimuthAngleDeg = 270.0; // Stbd azimuth angle (degrees, 270=ahead)
    double aziDriveLeverArm = 0.0;      // Distance from CG to azimuth drives along ship length (m, +ve = aft of CG)

    // Wind area parameters for Isherwood model (0 = use defaults from dims)
    double lateralWindArea = 0.0;  // Lateral projected area above waterline (m^2)
    double frontalWindArea = 0.0;  // Frontal/transverse projected area (m^2)
    double superstructureAft = 0.0; // Distance from bow to centroid of lateral area (m)
};

// Ship hull dimensions and characteristics
struct ShipDimensions {
    double length = 100.0;       // Length between perpendiculars (m)
    double beam = 16.0;          // Beam / width (m)
    double draught = 6.0;        // Draught / depth (m)
    double displacement = 0.0;   // Displacement (kg)
    double blockCoefficient = 0.8; // Block coefficient Cb
    double maxSpeed = 10.0;      // Maximum speed (m/s)
    double maxEngineForce = 1e6; // Maximum engine thrust (N)
    double propellerDiameter = 4.0; // Propeller diameter (m)
    double maxRPM = 120.0;      // Maximum propeller RPM
    bool singleEngine = false;   // True if single engine (vs twin screw)
    double propellorSpacing = 0.0; // Distance between propellers (m), 0 for single screw
};

// Abstract physics model interface
class IPhysicsModel {
public:
    virtual ~IPhysicsModel() = default;

    // Advance state by dt seconds
    virtual void step(double dt, const PhysicsInput& input, PhysicsState& state) = 0;

    // Get ship dimensions (for queries)
    virtual const ShipDimensions& getDimensions() const = 0;
};

#endif // __PHYSICSMODEL_HPP_INCLUDED__
