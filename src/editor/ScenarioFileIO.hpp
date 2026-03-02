#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "../ScenarioDataStructure.hpp"

// Lightweight buoy data for map display (lat/lon)
struct EditorBuoyData {
    float lat = 0, lon = 0;
    std::string typeName;
};

// Lightweight light data for map display
struct EditorLightData {
    float lat = 0, lon = 0;
    uint8_t r = 255, g = 255, b = 255;
    float range = 0;
    std::string sequence;
    int buoyIndex = 0; // 0 = standalone, >0 = on buoy N
};

class ScenarioFileIO {
public:
    // Save scenario to directory
    // Creates: environment.ini, ownship.ini, othership.ini, description.ini
    static bool save(const std::string& scenarioDir, const ScenarioData& data);

    // Load scenario from directory
    static ScenarioData load(const std::string& scenarioDir);

    // Load buoys from a world directory's buoy.ini
    static std::vector<EditorBuoyData> loadBuoys(const std::string& worldDir);

    // Load lights from a world directory's light.ini
    static std::vector<EditorLightData> loadLights(const std::string& worldDir);

    // List available scenarios (subdirectories of a Scenarios path)
    static std::vector<std::string> listScenarios(const std::string& scenariosDir);

    // List available worlds (subdirectories of a World path)
    static std::vector<std::string> listWorlds(const std::string& worldsDir);

    // List available ship models (subdirectories of a Models path)
    static std::vector<std::string> listShipModels(const std::string& modelsDir);
};
