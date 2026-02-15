#pragma once

// Entry point for running Bridge Command with the Wicked Engine backend.
// When --wicked is passed on the command line, the simulator uses
// Wicked Engine (DX12) instead of the default Irrlicht (OpenGL) renderer.

#ifdef WITH_WICKED_ENGINE

#include <string>
#include "ScenarioDataStructure.hpp"

// Runs the scenario using Wicked Engine as the rendering backend.
// Loads terrain, ships, buoys, and land objects from the scenario data.
// Returns process exit code.
int runWickedEngine(const std::string& userFolder, const ScenarioData& scenarioData,
                    int width, int height, bool fullscreen);

#endif // WITH_WICKED_ENGINE
