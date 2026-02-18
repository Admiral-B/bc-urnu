/*   Bridge Command - SimulationBridge
     Thin wrapper around SimulationModel for use by the Wicked Engine backend.
     This header contains NO Irrlicht types so it can be safely included
     alongside WickedEngine.h.  The implementation (SimulationBridge.cpp)
     is a separate compilation unit that includes irrlicht.h. */

#pragma once

#include <string>

// Forward declarations (no Irrlicht headers)
struct ScenarioData;
class ISound;

namespace SimBridge {

// Lifecycle
void init(ISound* sound, const ScenarioData& scenarioData);
void start();      // unpause physics timer -- call once before game loop
void syncTimer();  // flush Irrlicht timer -- call just before entering game loop
void update();     // advance physics one frame (call device->run + model.update)
void shutdown();   // destroy SimulationModel + headless Irrlicht device

// Controls (WickedMain -> SimulationModel)
void setPortEngine(float val);   // -1..+1
void setStbdEngine(float val);   // -1..+1
void setWheel(float val);        // degrees, -ve port
void setBowThruster(float val);  // -1..+1
void setSternThruster(float val);// -1..+1

// Own ship state
float getPosX();
float getPosZ();
float getHeading();      // degrees
float getSOG();          // m/s
float getSTW();          // m/s (speed through water)
float getCOG();          // degrees
float getDepth();        // metres below keel
float getPosY();         // vertical pos (tide + heightCorr + wave heave)
float getPitch();        // degrees
float getRoll();         // degrees
float getRudder();       // degrees
float getWheel();        // degrees
float getPortEngine();   // -1..+1
float getStbdEngine();   // -1..+1
float getPortEngineRPM();
float getStbdEngineRPM();
float getRateOfTurn();   // degrees/min
float getWeather();      // Beaufort 0-12
float getWindSpeed();    // knots
float getWindDirection(); // degrees

// Other ships
int getNumberOfOtherShips();
float getOtherShipPosX(int i);
float getOtherShipPosZ(int i);
float getOtherShipHeading(int i);
float getOtherShipSpeed(int i);  // m/s

// Buoys
int getNumberOfBuoys();
float getBuoyPosX(int i);
float getBuoyPosZ(int i);

// Time & lighting
float getTimeDelta();      // scenario time in seconds since midnight day 1
uint32_t getLightLevel();  // ambient light 0-255 (0=dark, 255=bright day)

} // namespace SimBridge
