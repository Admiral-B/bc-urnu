/*   Bridge Command - SimulationBridge implementation
     Separate compilation unit that includes irrlicht.h (must NOT include
     WickedEngine.h).  Creates a headless Irrlicht device solely for
     SimulationModel's physics / AI / terrain. */

#include "SimulationBridge.hpp"
#include "irrlicht.h"
#include "SimulationModel.hpp"
#include "ScenarioDataStructure.hpp"
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

void init(ISound* sound, const ScenarioData& scenarioData) {
    // Create headless Irrlicht device (no visible window, physics only)
    irr::SIrrlichtCreationParameters params;
    params.DriverType = irr::video::EDT_NULL;
    params.WindowSize = irr::core::dimension2d<irr::u32>(800, 600);
    params.Stencilbuffer = false;
    params.AntiAlias = 0;
    // Set a dummy WindowId so Irrlicht treats this as an "external window" device.
    // This prevents handleSystemMessages() from stealing WE's window messages
    // via PeekMessage(NULL) -- it will only process messages for HWnd (which is
    // this dummy value, so no real messages match).
    params.WindowId = reinterpret_cast<void*>(static_cast<uintptr_t>(1));

    g_device = irr::createDeviceEx(params);
    if (!g_device) {
        std::cerr << "SimulationBridge: Failed to create headless Irrlicht device" << std::endl;
        return;
    }
    // Pause timer during construction so first update() has a small deltaTime
    g_device->getTimer()->setSpeed(0.0f);

    g_smgr = g_device->getSceneManager();

    // Read bc5.ini for model parameters (same defaults as main.cpp)
    std::string userFolder = Utilities::getUserDir();
    std::string iniFilename = "bc5.ini";
    if (Utilities::pathExists(userFolder + iniFilename)) {
        iniFilename = userFolder + iniFilename;
    }

    SimulationModel::ModelParameters mp = {};
    mp.mode = OperatingMode::Normal;
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

    mp.secondaryControlWheel = false;
    mp.secondaryControlPortEngine = false;
    mp.secondaryControlStbdEngine = false;
    mp.secondaryControlPortSchottel = false;
    mp.secondaryControlStbdSchottel = false;
    mp.secondaryControlPortThrustLever = false;
    mp.secondaryControlStbdThrustLever = false;
    mp.secondaryControlBowThruster = false;
    mp.secondaryControlSternThruster = false;

    // Construct SimulationModel (loads terrain, ships, buoys, etc.)
    g_model = new SimulationModel(g_device, g_smgr, nullptr /*gui*/, sound,
                                  scenarioData, mp);

    std::cout << "SimulationBridge: initialized (headless Irrlicht + SimulationModel)" << std::endl;

    // Diagnostic: log initial state to help debug terrain/depth issues
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
    // Flush accumulated time on the Irrlicht timer.
    // Call just before the game loop to prevent a huge deltaTime
    // on the first physics frame (timer runs during scene setup).
    if (g_device) {
        g_device->run();
        g_device->run();
    }
}

void update() {
    if (!g_model || !g_device) return;
    g_device->run();        // advance Irrlicht timer (ExternalWindow mode = no message stealing)
    g_model->update();      // physics + AI + buoys + tide + ...
}

void shutdown() {
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

// Other ships
int getNumberOfOtherShips() {
    return g_model ? (int)g_model->getNumberOfOtherShips() : 0;
}
float getOtherShipPosX(int i)    { return g_model ? g_model->getOtherShipPosX(i) : 0; }
float getOtherShipPosZ(int i)    { return g_model ? g_model->getOtherShipPosZ(i) : 0; }
float getOtherShipHeading(int i) { return g_model ? g_model->getOtherShipHeading(i) : 0; }
float getOtherShipSpeed(int i)   { return g_model ? g_model->getOtherShipSpeed(i) : 0; }

// Buoys
int getNumberOfBuoys() {
    return g_model ? (int)g_model->getNumberOfBuoys() : 0;
}
float getBuoyPosX(int i) { return g_model ? g_model->getBuoyPosX(i) : 0; }
float getBuoyPosZ(int i) { return g_model ? g_model->getBuoyPosZ(i) : 0; }

// Time & lighting
float getTimeDelta()      { return g_model ? g_model->getTimeDelta() : 0; }
uint32_t getLightLevel()  { return g_model ? g_model->getLightLevel() : 200; }

} // namespace SimBridge
