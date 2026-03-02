/*   Bridge Command - SimulationBridge
     Thin wrapper around SimulationModel for use by the Wicked Engine backend.
     This header contains NO Irrlicht types so it can be safely included
     alongside WickedEngine.h.  The implementation (SimulationBridge.cpp)
     is a separate compilation unit that includes irrlicht.h. */

#pragma once

#include <string>
#include <cstdint>

// Forward declarations (no Irrlicht headers)
class ScenarioData;
class ISound;

namespace SimBridge {

// Lifecycle
void init(ISound* sound, const ScenarioData& scenarioData, int operatingMode = 0);
void start();      // unpause physics timer -- call once before game loop
void syncTimer();  // flush Irrlicht timer -- call just before entering game loop
void update();     // advance physics one frame (call device->run + model.update)
void shutdown();   // destroy SimulationModel + headless Irrlicht device + network

// Network (secondary mode)
// Call initNetwork() BEFORE init() for secondary mode.
// operatingMode: 0=Normal, 1=Secondary
void initNetwork(int operatingMode, int port, const std::string& hostname);
bool waitForScenario(ScenarioData& outScenarioData); // non-blocking poll; returns true when ready
int  getNetworkPort(); // returns the port Network is listening on (for display)

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
float getShipLength();   // metres (world scale)
float getShipBreadth();  // metres (world scale)
float getShipDraught();  // metres (world scale)
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
float getRain();         // 0-10 intensity
float getVisibility();   // nautical miles
float getTideHeight();   // metres

// Other ships
int getNumberOfOtherShips();
float getOtherShipPosX(int i);
float getOtherShipPosZ(int i);
float getOtherShipHeading(int i);
float getOtherShipSpeed(int i);  // m/s
std::string getOtherShipName(int i);
uint32_t getOtherShipMMSI(int i);
float getOtherShipLength(int i);
float getOtherShipBreadth(int i);
float getOtherShipPosY(int i);
float getOtherShipWavePitch(int i);
float getOtherShipWaveRoll(int i);

// Buoys
int getNumberOfBuoys();
float getBuoyPosX(int i);
float getBuoyPosZ(int i);

// Time & lighting
float getTimeDelta();      // scenario time in seconds since midnight day 1
uint32_t getLightLevel();  // ambient light 0-255 (0=dark, 255=bright day)
float getSunRise();        // hours (0-24)
float getSunSet();         // hours (0-24)

// Radar display -- copies the overlaid radar image (ARGB->RGBA) into outBuf.
// Downsamples to maxSize x maxSize if source is larger.
// outBuf must be at least maxSize*maxSize*4 bytes.
bool getRadarImage(uint8_t* outBuf, int maxSize = 512);
int  getRadarImageSize();  // source side length in pixels

// Radar controls
void increaseRadarRange();
void decreaseRadarRange();
float getRadarRangeNm();
void setRadarGain(float val);       // 0..1
void setRadarClutter(float val);    // 0..1
void setRadarRain(float val);       // 0..1
void setRadarNorthUp();
void setRadarCourseUp();
void setRadarHeadUp();
void setRadarARPARel();
void setRadarARPATrue();
void setRadarARPAVectors(float minutes);
void toggleRadarOn();
bool isRadarOn();
void setRadarCursorPosition(int relX, int relY); // pixels relative to radar center
void setRadarMouseDown(bool down);
void setArpaMode(int mode);         // 0=Manual, 1=ARPA, 2=Full ARPA
int  getArpaMode();
void trackTargetFromCursor();       // start tracking nearest contact to cursor
void addManualPoint(bool newContact); // manual ARPA: new or update selected
void clearAllArpaContacts();

// ARPA contacts
struct ARPADisplayContact {
    uint32_t displayID;
    float bearing;   // degrees
    float range;     // Nm
    float speed;     // knots
    float heading;   // degrees
    float cpa;       // Nm
    float tcpa;      // minutes
    bool lost;
    bool stationary;
};
int getARPATracksCount();
ARPADisplayContact getARPAContact(int index);

} // namespace SimBridge
