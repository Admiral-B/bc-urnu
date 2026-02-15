#include "ScenarioFileIO.hpp"
#include "../IniFile.hpp"
#include "../Utilities.hpp"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <cmath>
#include <filesystem>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

static const float SECONDS_IN_HOUR = 3600.0f;

bool ScenarioFileIO::save(const std::string& scenarioDir, const ScenarioData& data) {

    // Create directory if it doesn't exist
    if (!Utilities::pathExists(scenarioDir)) {
#ifdef _WIN32
        _mkdir(scenarioDir.c_str());
#else
        mkdir(scenarioDir.c_str(), 0755);
#endif
    }

    bool success = true;

    // environment.ini
    {
        std::ofstream f(scenarioDir + "/environment.ini");
        f << "Setting=\"" << data.worldName << "\"" << std::endl;
        f << "StartTime=" << data.startTime / SECONDS_IN_HOUR << std::endl;
        f << "StartDay=" << data.startDay << std::endl;
        f << "StartMonth=" << data.startMonth << std::endl;
        f << "StartYear=" << data.startYear << std::endl;
        f << "SunRise=" << data.sunRise << std::endl;
        f << "SunSet=" << data.sunSet << std::endl;
        f << "VisibilityRange=" << data.visibilityRange << std::endl;
        f << "Weather=" << data.weather << std::endl;
        f << "WindDirection=" << data.windDirection << std::endl;
        f << "WindSpeed=" << data.windSpeed << std::endl;
        f << "Rain=" << data.rainIntensity << std::endl;
        f.close();
        if (!f.good()) success = false;
    }

    // ownship.ini
    {
        std::ofstream f(scenarioDir + "/ownship.ini");
        f << "ShipName=\"" << data.ownShipData.ownShipName << "\"" << std::endl;
        f << "InitialLong=" << std::setprecision(8) << data.ownShipData.initialLong << std::endl;
        f << "InitialLat=" << std::setprecision(8) << data.ownShipData.initialLat << std::endl;
        f << "InitialBearing=" << std::setprecision(8) << data.ownShipData.initialBearing << std::endl;
        f << "InitialSpeed=" << std::setprecision(8) << data.ownShipData.initialSpeed << std::endl;
        f.close();
        if (!f.good()) success = false;
    }

    // othership.ini
    {
        std::ofstream f(scenarioDir + "/othership.ini");
        f << "Number=" << data.otherShipsData.size() << std::endl;
        for (size_t i = 0; i < data.otherShipsData.size(); i++) {
            int idx = (int)i + 1; // 1-based index in file format
            const auto& ship = data.otherShipsData[i];
            f << "Type(" << idx << ")=\"" << ship.shipName << "\"" << std::endl;
            f << "InitLong(" << idx << ")=" << std::setprecision(8) << ship.initialLong << std::endl;
            f << "InitLat(" << idx << ")=" << std::setprecision(8) << ship.initialLat << std::endl;
            f << "mmsi(" << idx << ")=" << ship.mmsi << std::endl;
            if (ship.drifting) {
                f << "Drifting(" << idx << ")=1" << std::endl;
            }
            // Don't save the last leg (automatically added 'stop' leg)
            int legCount = (int)ship.legs.size() - 1;
            if (legCount < 0) legCount = 0;
            f << "Legs(" << idx << ")=" << legCount << std::endl;
            for (int j = 0; j < legCount; j++) {
                int legIdx = j + 1; // 1-based
                const auto& leg = ship.legs[j];
                f << "Bearing(" << idx << "," << legIdx << ")=" << std::setprecision(8) << leg.bearing << std::endl;
                f << "Speed(" << idx << "," << legIdx << ")=" << std::setprecision(8) << leg.speed << std::endl;
                f << "Distance(" << idx << "," << legIdx << ")=" << std::setprecision(8) << leg.distance << std::endl;
            }
        }
        f.close();
        if (!f.good()) success = false;
    }

    // description.ini (plain text, not INI format)
    {
        std::ofstream f(scenarioDir + "/description.ini");
        f << data.description;
        f.close();
        if (!f.good()) success = false;
    }

    return success;
}

ScenarioData ScenarioFileIO::load(const std::string& scenarioDir) {
    ScenarioData data;

    std::string envFile = scenarioDir + "/environment.ini";
    std::string ownFile = scenarioDir + "/ownship.ini";
    std::string otherFile = scenarioDir + "/othership.ini";
    std::string descFile = scenarioDir + "/description.ini";

    // Environment
    data.worldName = IniFile::iniFileToString(envFile, "Setting");
    data.startTime = SECONDS_IN_HOUR * IniFile::iniFileTof32(envFile, "StartTime");
    data.startDay = IniFile::iniFileTou32(envFile, "StartDay");
    data.startMonth = IniFile::iniFileTou32(envFile, "StartMonth");
    data.startYear = IniFile::iniFileTou32(envFile, "StartYear");
    data.sunRise = IniFile::iniFileTof32(envFile, "SunRise");
    data.sunSet = IniFile::iniFileTof32(envFile, "SunSet");
    data.weather = IniFile::iniFileTof32(envFile, "Weather");
    data.visibilityRange = IniFile::iniFileTof32(envFile, "VisibilityRange");
    data.rainIntensity = IniFile::iniFileTof32(envFile, "Rain");
    data.windDirection = IniFile::iniFileTof32(envFile, "WindDirection");
    data.windSpeed = IniFile::iniFileTof32(envFile, "WindSpeed");

    // Defaults for sun
    if (data.sunRise == 0.0f) data.sunRise = 6.0f;
    if (data.sunSet == 0.0f) data.sunSet = 18.0f;

    // Own ship
    data.ownShipData.ownShipName = IniFile::iniFileToString(ownFile, "ShipName");
    data.ownShipData.initialLong = IniFile::iniFileTof32(ownFile, "InitialLong");
    data.ownShipData.initialLat = IniFile::iniFileTof32(ownFile, "InitialLat");
    data.ownShipData.initialBearing = IniFile::iniFileTof32(ownFile, "InitialBearing");
    data.ownShipData.initialSpeed = IniFile::iniFileTof32(ownFile, "InitialSpeed");

    // Other ships
    int numberOfShips = IniFile::iniFileTou32(otherFile, "Number");
    for (int i = 1; i <= numberOfShips; i++) {
        OtherShipData ship;
        ship.initialLong = IniFile::iniFileTof32(otherFile, IniFile::enumerate1("InitLong", i));
        ship.initialLat = IniFile::iniFileTof32(otherFile, IniFile::enumerate1("InitLat", i));
        ship.shipName = IniFile::iniFileToString(otherFile, IniFile::enumerate1("Type", i));
        ship.mmsi = IniFile::iniFileTou32(otherFile, IniFile::enumerate1("mmsi", i));
        ship.drifting = (IniFile::iniFileTou32(otherFile, IniFile::enumerate1("Drifting", i)) == 1);

        int numberOfLegs = (int)IniFile::iniFileTof32(otherFile, IniFile::enumerate1("Legs", i));
        float legStartTime = data.startTime;
        for (int j = 1; j <= numberOfLegs; j++) {
            LegData leg;
            leg.bearing = IniFile::iniFileTof32(otherFile, IniFile::enumerate2("Bearing", i, j));
            leg.speed = IniFile::iniFileTof32(otherFile, IniFile::enumerate2("Speed", i, j));
            leg.distance = IniFile::iniFileTof32(otherFile, IniFile::enumerate2("Distance", i, j));
            leg.startTime = legStartTime;
            ship.legs.push_back(leg);
            // Calculate start time for next leg
            if (std::fabs(leg.speed) > 0.001f) {
                legStartTime += SECONDS_IN_HOUR * (leg.distance / std::fabs(leg.speed));
            }
        }

        // Add final 'stop' leg
        LegData stopLeg;
        stopLeg.bearing = 0;
        stopLeg.speed = 0;
        stopLeg.distance = 0;
        stopLeg.startTime = legStartTime;
        ship.legs.push_back(stopLeg);

        data.otherShipsData.push_back(std::move(ship));
    }

    // Description (plain text file)
    {
        std::ifstream f(descFile);
        if (f.is_open()) {
            std::string line;
            while (std::getline(f, line)) {
                data.description += line;
                data.description += "\n";
            }
        }
    }

    data.dataPopulated = true;
    return data;
}

std::vector<EditorBuoyData> ScenarioFileIO::loadBuoys(const std::string& worldDir) {
    std::vector<EditorBuoyData> result;
    std::string buoyFile = worldDir + "/buoy.ini";
    int count = IniFile::iniFileTou32(buoyFile, "Number");
    for (int i = 1; i <= count; i++) {
        EditorBuoyData b;
        b.lat = IniFile::iniFileTof32(buoyFile, IniFile::enumerate1("Lat", i));
        b.lon = IniFile::iniFileTof32(buoyFile, IniFile::enumerate1("Long", i));
        b.typeName = IniFile::iniFileToString(buoyFile, IniFile::enumerate1("Type", i));
        result.push_back(std::move(b));
    }
    return result;
}

std::vector<EditorLightData> ScenarioFileIO::loadLights(const std::string& worldDir) {
    std::vector<EditorLightData> result;
    std::string lightFile = worldDir + "/light.ini";
    int count = IniFile::iniFileTou32(lightFile, "Number");
    for (int i = 1; i <= count; i++) {
        EditorLightData l;
        l.lat = IniFile::iniFileTof32(lightFile, IniFile::enumerate1("Lat", i));
        l.lon = IniFile::iniFileTof32(lightFile, IniFile::enumerate1("Long", i));
        l.r = (uint8_t)IniFile::iniFileTou32(lightFile, IniFile::enumerate1("Red", i));
        l.g = (uint8_t)IniFile::iniFileTou32(lightFile, IniFile::enumerate1("Green", i));
        l.b = (uint8_t)IniFile::iniFileTou32(lightFile, IniFile::enumerate1("Blue", i));
        l.range = IniFile::iniFileTof32(lightFile, IniFile::enumerate1("Range", i));
        l.sequence = IniFile::iniFileToString(lightFile, IniFile::enumerate1("Sequence", i));
        l.buoyIndex = (int)IniFile::iniFileTou32(lightFile, IniFile::enumerate1("Buoy", i));
        result.push_back(std::move(l));
    }
    return result;
}

std::vector<std::string> ScenarioFileIO::listScenarios(const std::string& scenariosDir) {
    std::vector<std::string> result;
    try {
        for (const auto& entry : std::filesystem::directory_iterator(scenariosDir)) {
            if (entry.is_directory()) {
                std::string name = entry.path().filename().string();
                if (!name.empty() && name[0] != '.') {
                    result.push_back(name);
                }
            }
        }
    } catch (...) {}
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<std::string> ScenarioFileIO::listWorlds(const std::string& worldsDir) {
    std::vector<std::string> result;
    try {
        for (const auto& entry : std::filesystem::directory_iterator(worldsDir)) {
            if (entry.is_directory()) {
                std::string name = entry.path().filename().string();
                if (!name.empty() && name[0] != '.') {
                    result.push_back(name);
                }
            }
        }
    } catch (...) {}
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<std::string> ScenarioFileIO::listShipModels(const std::string& modelsDir) {
    std::vector<std::string> result;
    try {
        for (const auto& entry : std::filesystem::directory_iterator(modelsDir)) {
            if (entry.is_directory()) {
                std::string name = entry.path().filename().string();
                if (!name.empty() && name[0] != '.') {
                    result.push_back(name);
                }
            }
        }
    } catch (...) {}
    std::sort(result.begin(), result.end());
    return result;
}
