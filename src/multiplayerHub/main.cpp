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

// main.cpp - ImGui-based Multiplayer Hub
// Default: server mode (listens for connections)
// --legacy: client mode (connects out to peers, original behavior)

#include "HubApp.hpp"
#include "../IniFile.hpp"

#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#ifdef _MSC_VER
#pragma comment(linker, "/subsystem:windows /ENTRY:mainCRTStartup")
#endif

// Stub for IniFile's Irrlicht logger dependency (not used in ImGui build)
namespace irr { class ILogger; }
namespace IniFile {
    irr::ILogger* irrlichtLogger = 0;
}

int main(int argc, char* argv[])
{
    bool legacyMode = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--legacy") == 0) {
            legacyMode = true;
        }
    }

    HubApp app;
    if (!app.init(900, 600, legacyMode)) {
        return 1;
    }

    app.run();
    app.shutdown();

    return 0;
}
