/*   Bridge Command - SimulationBridge implementation
     Separate compilation unit that includes irrlicht.h (must NOT include
     WickedEngine.h).  Creates a headless Irrlicht device solely for
     SimulationModel's physics / AI / terrain. */

#include "SimulationBridge.hpp"
#include "irrlicht.h"
#include "SimulationModel.hpp"
#include "ScenarioDataStructure.hpp"
#include "Network.hpp"
#include "IniFile.hpp"
#include "Utilities.hpp"
#include "Constants.hpp"
#include "OperatingModeEnum.hpp"

#include <iostream>
#include <memory>

namespace SimBridge {

static irr::IrrlichtDevice* g_device = nullptr;
static irr::scene::ISceneManager* g_smgr = nullptr;
static SimulationModel* g_model = nullptr;
static Network* g_network = nullptr;
static OperatingMode::Mode g_mode = OperatingMode::Normal;
static float g_sunRise = 6.0f;
static float g_sunSet = 18.0f;

// Create headless Irrlicht device (shared by init and initNetwork)
static void ensureDevice() {
    if (g_device) return;
    irr::SIrrlichtCreationParameters params;
    params.DriverType = irr::video::EDT_NULL;
    params.WindowSize = irr::core::dimension2d<irr::u32>(800, 600);
    params.Stencilbuffer = false;
    params.AntiAlias = 0;
    params.WindowId = reinterpret_cast<void*>(static_cast<uintptr_t>(1));
    g_device = irr::createDeviceEx(params);
    if (!g_device) {
        std::cerr << "SimulationBridge: Failed to create headless Irrlicht device" << std::endl;
        return;
    }
    g_device->getTimer()->setSpeed(0.0f);
    g_smgr = g_device->getSceneManager();
}

void initNetwork(int operatingMode, int port, const std::string& hostname) {
    ensureDevice();
    if (!g_device) return;

    g_mode = (operatingMode == 1) ? OperatingMode::Secondary : OperatingMode::Normal;
    g_network = Network::createNetwork(g_mode, port, g_device);
    if (g_mode == OperatingMode::Normal) {
        // Primary: connect to secondary hostnames
        g_network->connectToServer(hostname);
    }
    // Secondary: server listens, no connectToServer needed
    // (primary connects to us)
    std::cout << "SimulationBridge: network initialized (mode="
              << operatingMode << " port=" << g_network->getPort() << ")" << std::endl;
}

bool waitForScenario(ScenarioData& outScenarioData) {
    if (!g_network) return false;
    std::string data;
    g_network->getScenarioFromNetwork(data);
    if (data.empty()) return false;
    outScenarioData.deserialise(data);
    std::cout << "SimulationBridge: received scenario '" << outScenarioData.scenarioName
              << "' world='" << outScenarioData.worldName << "'" << std::endl;
    return true;
}

int getNetworkPort() {
    return g_network ? g_network->getPort() : 0;
}

void init(ISound* sound, const ScenarioData& scenarioData, int operatingMode) {
    ensureDevice();
    if (!g_device) return;

    // Update mode if passed explicitly (may differ from initNetwork call)
    if (operatingMode == 1)
        g_mode = OperatingMode::Secondary;

    // Read bc5.ini for model parameters (same defaults as main.cpp)
    std::string userFolder = Utilities::getUserDir();
    std::string iniFilename = "bc5.ini";
    if (Utilities::pathExists(userFolder + iniFilename)) {
        iniFilename = userFolder + iniFilename;
    }

    SimulationModel::ModelParameters mp = {};
    mp.mode = g_mode;
    mp.vrMode = false;

    float viewAngle = IniFile::iniFileTof32(iniFilename, "view_angle");
    if (viewAngle <= 0) viewAngle = 90.0f;
    mp.viewAngle = viewAngle;
    mp.lookAngle = IniFile::iniFileTof32(iniFilename, "look_angle");

    float cameraMinDistance = IniFile::iniFileTof32(iniFilename, "minimum_distance");
    if (cameraMinDistance <= 0) cameraMinDistance = 1.0f;
    mp.cameraMinDistance = cameraMinDistance;

    float cameraMaxDistance = IniFile::iniFileTof32(iniFilename, "maximum_distance");
    if (cameraMaxDistance <= 0) cameraMaxDistance = 6.0f * M_IN_NM;
    mp.cameraMaxDistance = cameraMaxDistance;

    mp.disableShaders = IniFile::iniFileTou32(iniFilename, "disable_shaders");
    mp.waterSegments = IniFile::iniFileTou32(iniFilename, "water_segments");
    if (mp.waterSegments == 0) mp.waterSegments = 32;

    uint32_t cpX = IniFile::iniFileTou32(iniFilename, "contact_points_X");
    uint32_t cpY = IniFile::iniFileTou32(iniFilename, "contact_points_Y");
    uint32_t cpZ = IniFile::iniFileTou32(iniFilename, "contact_points_Z");
    if (cpX == 0) cpX = 10;
    if (cpY == 0) cpY = 30;
    if (cpZ == 0) cpZ = 30;
    mp.numberOfContactPoints = bc::graphics::Vec3i(cpX, cpY, cpZ);

    mp.minContactPointSpacing = IniFile::iniFileTof32(iniFilename, "contact_points_minSpacing", 100);
    mp.contactStiffnessFactor = IniFile::iniFileTof32(iniFilename, "contactStiffness_perArea");
    mp.contactDampingFactor = IniFile::iniFileTof32(iniFilename, "contactDamping_factor");
    mp.lineStiffnessFactor = IniFile::iniFileTof32(iniFilename, "lineStiffness_factor", 1.0f);
    mp.lineDampingFactor = IniFile::iniFileTof32(iniFilename, "lineDamping_factor", 1.0f);
    mp.frictionCoefficient = IniFile::iniFileTof32(iniFilename, "contactFriction_coefficient", 0.5f);
    mp.tanhFrictionFactor = IniFile::iniFileTof32(iniFilename, "contactFriction_tanhFactor", 1.0f);
    if (mp.frictionCoefficient < 0) mp.frictionCoefficient = 0;
    if (mp.frictionCoefficient > 1) mp.frictionCoefficient = 1;
    if (mp.tanhFrictionFactor < 0) mp.tanhFrictionFactor = 0;

    mp.limitTerrainResolution = IniFile::iniFileTou32(iniFilename, "max_terrain_resolution");
    mp.debugMode = (IniFile::iniFileTou32(iniFilename, "debug_mode") == 1);

    // Secondary control overrides (read from INI for secondary mode)
    mp.secondaryControlWheel = false;
    mp.secondaryControlPortEngine = false;
    mp.secondaryControlStbdEngine = false;
    mp.secondaryControlPortSchottel = false;
    mp.secondaryControlStbdSchottel = false;
    mp.secondaryControlPortThrustLever = false;
    mp.secondaryControlStbdThrustLever = false;
    mp.secondaryControlBowThruster = false;
    mp.secondaryControlSternThruster = false;
    if (g_mode == OperatingMode::Secondary) {
        if (IniFile::iniFileTou32(iniFilename, "secondary_control_wheel") == 1)
            mp.secondaryControlWheel = true;
        if (IniFile::iniFileTou32(iniFilename, "secondary_control_port_engine") == 1)
            mp.secondaryControlPortEngine = true;
        if (IniFile::iniFileTou32(iniFilename, "secondary_control_stbd_engine") == 1)
            mp.secondaryControlStbdEngine = true;
        if (IniFile::iniFileTou32(iniFilename, "secondary_control_port_schottel") == 1)
            mp.secondaryControlPortSchottel = true;
        if (IniFile::iniFileTou32(iniFilename, "secondary_control_stbd_schottel") == 1)
            mp.secondaryControlStbdSchottel = true;
        if (IniFile::iniFileTou32(iniFilename, "secondary_control_port_thrust") == 1)
            mp.secondaryControlPortThrustLever = true;
        if (IniFile::iniFileTou32(iniFilename, "secondary_control_stbd_thrust") == 1)
            mp.secondaryControlStbdThrustLever = true;
        if (IniFile::iniFileTou32(iniFilename, "secondary_control_bow_thruster") == 1)
            mp.secondaryControlBowThruster = true;
        if (IniFile::iniFileTou32(iniFilename, "secondary_control_stern_thruster") == 1)
            mp.secondaryControlSternThruster = true;
    }

    // Store sunrise/sunset for weather system
    g_sunRise = scenarioData.sunRise > 0 ? scenarioData.sunRise : 6.0f;
    g_sunSet  = scenarioData.sunSet  > 0 ? scenarioData.sunSet  : 18.0f;

    // Construct SimulationModel (loads terrain, ships, buoys, etc.)
    g_model = new SimulationModel(g_device, g_smgr, nullptr /*gui*/, sound,
                                  scenarioData, mp);

    // Connect network to model (if network was initialized first)
    if (g_network && g_model) {
        g_network->setModel(g_model);
    }

    std::cout << "SimulationBridge: initialized (headless Irrlicht + SimulationModel, mode="
              << (int)g_mode << ")" << std::endl;

    // Diagnostic: log initial state
    if (g_model) {
        std::cout << "SimulationBridge: initial pos=(" << g_model->getPosX()
                  << "," << g_model->getPosZ() << ") hdg=" << g_model->getHeading()
                  << " depth=" << g_model->getDepth()
                  << " posY=" << g_model->getPosY() << std::endl;
    }
}

void start() {
    if (g_device) {
        g_device->getTimer()->setSpeed(1.0f);
        g_device->run(); // kick timer (safe: ExternalWindow mode filters messages)
    }
    // Run one update so depth/position have meaningful values
    if (g_model && g_device) {
        g_device->run();
        g_model->update();
        std::cout << "SimulationBridge::start() pos=(" << g_model->getPosX()
                  << "," << g_model->getPosZ() << ") depth=" << g_model->getDepth()
                  << " posY=" << g_model->getPosY()
                  << " SOG=" << g_model->getSOG() << std::endl;
    }
}

void syncTimer() {
    if (g_device) {
        g_device->run();
        g_device->run();
    }
}

void update() {
    if (!g_model || !g_device) return;
    g_device->run();        // advance Irrlicht timer
    if (g_network) {
        g_network->update(); // receive/send network data (updates model in secondary mode)
    }
    g_model->update();      // physics + AI + buoys + tide + radar
}

void shutdown() {
    delete g_network;
    g_network = nullptr;
    delete g_model;
    g_model = nullptr;
    if (g_device) {
        g_device->drop();
        g_device = nullptr;
        g_smgr = nullptr;
    }
    std::cout << "SimulationBridge: shutdown complete" << std::endl;
}

// Controls
void setPortEngine(float val)    { if (g_model) g_model->setPortEngine(val); }
void setStbdEngine(float val)    { if (g_model) g_model->setStbdEngine(val); }
void setWheel(float val)         { if (g_model) g_model->setWheel(val); }
void setBowThruster(float val)   { if (g_model) g_model->setBowThruster(val); }
void setSternThruster(float val) { if (g_model) g_model->setSternThruster(val); }

// Own ship state
float getPosX()           { return g_model ? g_model->getPosX() : 0; }
float getPosZ()           { return g_model ? g_model->getPosZ() : 0; }
float getHeading()        { return g_model ? g_model->getHeading() : 0; }
float getSOG()            { return g_model ? g_model->getSOG() : 0; }
float getSTW()            { return g_model ? g_model->getSTW() : 0; }
float getCOG()            { return g_model ? g_model->getCOG() : 0; }
float getDepth()          { return g_model ? g_model->getDepth() : 0; }
float getPosY()           { return g_model ? g_model->getPosY() : 0; }
float getPitch()          { return g_model ? g_model->getPitch() : 0; }
float getRoll()           { return g_model ? g_model->getRoll() : 0; }
float getShipLength()     { return g_model ? g_model->getOwnShipLength() : 50; }
float getShipBreadth()    { return g_model ? g_model->getOwnShipBreadth() : 10; }
float getShipDraught()    { return g_model ? g_model->getOwnShipDraught() : 3; }
float getRudder()         { return g_model ? g_model->getRudder() : 0; }
float getWheel()          { return g_model ? g_model->getWheel() : 0; }
float getPortEngine()     { return g_model ? g_model->getPortEngine() : 0; }
float getStbdEngine()     { return g_model ? g_model->getStbdEngine() : 0; }
float getPortEngineRPM()  { return g_model ? g_model->getPortEngineRPM() : 0; }
float getStbdEngineRPM()  { return g_model ? g_model->getStbdEngineRPM() : 0; }
float getRateOfTurn()     { return g_model ? g_model->getRateOfTurn() : 0; }
float getWeather()        { return g_model ? g_model->getWeather() : 3; }
float getWindSpeed()      { return g_model ? g_model->getWindSpeed() : 0; }
float getWindDirection()  { return g_model ? g_model->getWindDirection() : 0; }
float getRain()           { return g_model ? g_model->getRain() : 0; }
float getVisibility()     { return g_model ? g_model->getVisibility() : 10; }
float getTideHeight()     { return g_model ? g_model->getTideHeight() : 0; }

// Other ships
int getNumberOfOtherShips() {
    return g_model ? (int)g_model->getNumberOfOtherShips() : 0;
}
float getOtherShipPosX(int i)    { return g_model ? g_model->getOtherShipPosX(i) : 0; }
float getOtherShipPosZ(int i)    { return g_model ? g_model->getOtherShipPosZ(i) : 0; }
float getOtherShipHeading(int i) { return g_model ? g_model->getOtherShipHeading(i) : 0; }
float getOtherShipSpeed(int i)   { return g_model ? g_model->getOtherShipSpeed(i) : 0; }
std::string getOtherShipName(int i) { return g_model ? g_model->getOtherShipName(i) : ""; }
uint32_t getOtherShipMMSI(int i) { return g_model ? g_model->getOtherShipMMSI(i) : 0; }
float getOtherShipLength(int i)  { return g_model ? g_model->getOtherShipLength(i) : 0; }
float getOtherShipBreadth(int i) { return g_model ? g_model->getOtherShipBreadth(i) : 0; }
float getOtherShipPosY(int i)      { return g_model ? g_model->getOtherShipPosY(i) : 0; }
float getOtherShipWavePitch(int i) { return g_model ? g_model->getOtherShipWavePitch(i) : 0; }
float getOtherShipWaveRoll(int i)  { return g_model ? g_model->getOtherShipWaveRoll(i) : 0; }

// Buoys
int getNumberOfBuoys() {
    return g_model ? (int)g_model->getNumberOfBuoys() : 0;
}
float getBuoyPosX(int i) { return g_model ? g_model->getBuoyPosX(i) : 0; }
float getBuoyPosZ(int i) { return g_model ? g_model->getBuoyPosZ(i) : 0; }

// Time & lighting
float getTimeDelta()      { return g_model ? g_model->getTimeDelta() : 0; }
uint32_t getLightLevel()  { return g_model ? g_model->getLightLevel() : 200; }
float getSunRise()        { return g_sunRise; }
float getSunSet()         { return g_sunSet; }

// Radar display
bool getRadarImage(uint8_t* outBuf, int maxSize) {
    if (!g_model || !outBuf || maxSize <= 0) return false;
    irr::video::IImage* img = g_model->getRadarImageOverlaid();
    if (!img) return false;
    uint32_t w = img->getDimension().Width;
    uint32_t h = img->getDimension().Height;
    if (w == 0 || h == 0) return false;
    const uint32_t* src = reinterpret_cast<const uint32_t*>(img->getData());
    if (!src) return false;

    static bool logged = false;
    if (!logged) {
        std::cout << "SimBridge::getRadarImage: src=" << w << "x" << h
                  << " dst=" << maxSize << "x" << maxSize << std::endl;
        logged = true;
    }

    uint32_t outW = (uint32_t)maxSize;
    for (uint32_t oy = 0; oy < outW; oy++) {
        uint32_t sy = oy * h / outW;
        if (sy >= h) sy = h - 1;
        for (uint32_t ox = 0; ox < outW; ox++) {
            uint32_t sx = ox * w / outW;
            if (sx >= w) sx = w - 1;
            uint32_t argb = src[sy * w + sx];
            uint32_t idx = (oy * outW + ox) * 4;
            outBuf[idx+0] = (argb >> 16) & 0xFF; // R
            outBuf[idx+1] = (argb >>  8) & 0xFF; // G
            outBuf[idx+2] = (argb      ) & 0xFF; // B
            outBuf[idx+3] = (argb >> 24) & 0xFF; // A
        }
    }
    return true;
}

int getRadarImageSize() {
    if (!g_model) return 0;
    irr::video::IImage* img = g_model->getRadarImageOverlaid();
    if (!img) return 0;
    uint32_t w = img->getDimension().Width;
    return (w > 0 && w <= 16384) ? (int)w : 0;
}

// Radar controls
void increaseRadarRange()            { if (g_model) g_model->increaseRadarRange(); }
void decreaseRadarRange()            { if (g_model) g_model->decreaseRadarRange(); }
float getRadarRangeNm()              { if (!g_model) return 0; return g_model->getRadarRangeNm(); }
void setRadarGain(float val)         { if (g_model) g_model->setRadarGain(val); }
void setRadarClutter(float val)      { if (g_model) g_model->setRadarClutter(val); }
void setRadarRain(float val)         { if (g_model) g_model->setRadarRain(val); }
void setRadarNorthUp()               { if (g_model) g_model->setRadarNorthUp(); }
void setRadarCourseUp()              { if (g_model) g_model->setRadarCourseUp(); }
void setRadarHeadUp()                { if (g_model) g_model->setRadarHeadUp(); }
void setRadarARPARel()               { if (g_model) g_model->setRadarARPARel(); }
void setRadarARPATrue()              { if (g_model) g_model->setRadarARPATrue(); }
void setRadarARPAVectors(float min)  { if (g_model) g_model->setRadarARPAVectors(min); }
void toggleRadarOn()                 { if (g_model) g_model->toggleRadarOn(); }
bool isRadarOn()                     { return g_model ? g_model->isRadarOn() : false; }
void setRadarCursorPosition(int relX, int relY) {
    if (g_model) g_model->setRadarCursorPosition(irr::core::vector2di(relX, relY));
}
void setRadarMouseDown(bool down) {
    if (g_model) g_model->setMouseDown(down);
}
void setArpaMode(int mode)           { if (g_model) g_model->setArpaMode(mode); }
int  getArpaMode()                   { return g_model ? g_model->getArpaMode() : 0; }
void trackTargetFromCursor()         { if (g_model) g_model->trackTargetFromCursor(); }
void addManualPoint(bool newContact) { if (g_model) g_model->addManualPoint(newContact); }
void clearAllArpaContacts()          { if (g_model) g_model->clearManualPoints(); }

// ARPA contacts
int getARPATracksCount() {
    return g_model ? (int)g_model->getARPATracksSize() : 0;
}

ARPADisplayContact getARPAContact(int index) {
    ARPADisplayContact c = {};
    if (!g_model || index < 0 || index >= (int)g_model->getARPATracksSize()) return c;
    ARPAContact ac = g_model->getARPAContactFromTrackIndex((uint32_t)index);
    c.displayID = ac.estimate.displayID;
    c.bearing = ac.estimate.bearing;
    c.range = ac.estimate.range;
    c.speed = ac.estimate.speed;
    c.heading = ac.estimate.absHeading;
    c.cpa = ac.estimate.cpa;
    c.tcpa = ac.estimate.tcpa;
    c.lost = ac.estimate.lost;
    c.stationary = ac.estimate.stationary;
    return c;
}

} // namespace SimBridge
