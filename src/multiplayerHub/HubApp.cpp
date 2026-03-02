#include "HubApp.hpp"
#include "Network.hpp"
#include "ShipPositions.hpp"
#include "LinesData.hpp"
#include "../Utilities.hpp"
#include "../Constants.hpp"
#include "../IniFile.hpp"

#ifdef _WIN32
#include <direct.h>
#endif

// ImGui core
#include "../graphics/wicked/imgui/imgui.h"

// ImGui backends
#ifdef _WIN32
#include "../graphics/wicked/imgui/backends/imgui_impl_win32.h"
#include "../graphics/wicked/imgui/backends/imgui_impl_opengl3.h"
#include <GL/gl.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
#endif

#include <iostream>
#include <fstream>
#include <filesystem>
#include <algorithm>

// ---- Time string helper (same format as original hub) ----
static std::string makeTimeString(uint64_t absoluteTime, uint64_t offsetTime, float scenarioTime, float accelerator) {
    std::string s = Utilities::lexical_cast<std::string>(absoluteTime);
    s += ",";
    s += Utilities::lexical_cast<std::string>(offsetTime);
    s += ",";
    s += Utilities::lexical_cast<std::string>(scenarioTime);
    s += ",";
    s += Utilities::lexical_cast<std::string>(accelerator);
    return s;
}

// ---- Platform: window creation and GL context ----

#ifdef _WIN32

bool HubApp::createWindow(int width, int height) {
    wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"BCHubClass";

    if (!RegisterClassExW(&wc)) {
        std::cerr << "HubApp: Failed to register window class" << std::endl;
        return false;
    }

    RECT wr = { 0, 0, width, height };
    AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, 0);

    hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"Multiplayer Hub",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        wr.right - wr.left, wr.bottom - wr.top,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!hwnd) {
        std::cerr << "HubApp: Failed to create window" << std::endl;
        return false;
    }

    hdc = GetDC(hwnd);
    windowWidth = width;
    windowHeight = height;
    return true;
}

bool HubApp::createGLContext() {
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;

    int pixelFormat = ChoosePixelFormat(hdc, &pfd);
    if (!pixelFormat) return false;
    if (!SetPixelFormat(hdc, pixelFormat, &pfd)) return false;

    hglrc = wglCreateContext(hdc);
    if (!hglrc) return false;
    if (!wglMakeCurrent(hdc, hglrc)) return false;

    return true;
}

void HubApp::destroyGLContext() {
    if (hglrc) {
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(hglrc);
        hglrc = nullptr;
    }
    if (hdc && hwnd) {
        ReleaseDC(hwnd, hdc);
        hdc = nullptr;
    }
}

LRESULT CALLBACK HubApp::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        return 0;
    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

#endif // _WIN32

// ---- Init / Shutdown ----

bool HubApp::init(int width, int height, bool legacy) {
    legacyMode = legacy;

    if (!createWindow(width, height))
        return false;
    if (!createGLContext())
        return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.FrameRounding = 2.0f;
    style.GrabRounding = 2.0f;
    style.WindowBorderSize = 1.0f;

#ifdef _WIN32
    ImGui_ImplWin32_Init(hwnd);
#endif
    ImGui_ImplOpenGL3_Init("#version 130");

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // Read settings
    userFolder = Utilities::getUserDir();
    std::string iniFilename = "mph.ini";
    if (Utilities::pathExists(userFolder + iniFilename)) {
        iniFilename = userFolder + iniFilename;
    }

    port = IniFile::iniFileTou32(iniFilename, "udp_send_port");
    if (port == 0) port = 18305;

    sleepTime = IniFile::iniFileTou32(iniFilename, "update_time");
    if (sleepTime == 0) sleepTime = 100;
    if (sleepTime > 10000) sleepTime = 10000;

    // Scenario path
    if (Utilities::pathExists(userFolder + "Scenarios/")) {
        scenarioPath = userFolder + "Scenarios/";
    } else {
        scenarioPath = "Scenarios/";
    }

    // Load saved hostname (for legacy mode)
    if (legacyMode && Utilities::pathExists(userFolder + "/hostname-mh.txt")) {
        std::string saved = IniFile::iniFileToString(userFolder + "/hostname-mh.txt", "hostname");
        strncpy_s(hostnameBuf, sizeof(hostnameBuf), saved.c_str(), _TRUNCATE);
    }

    refreshScenarioList();

    // Server mode: create network and start listening immediately
    if (!legacyMode) {
        network = new Network(port, true);
        network->startServer();
        statusMessage = "Listening on port " + std::to_string(port) + ". Waiting for players...";
    }

    running = true;
    phase = LOBBY;
    return true;
}

void HubApp::shutdown() {
    delete network; network = nullptr;
    delete shipPositions; shipPositions = nullptr;
    delete linesData; linesData = nullptr;

    ImGui_ImplOpenGL3_Shutdown();
#ifdef _WIN32
    ImGui_ImplWin32_Shutdown();
#endif
    ImGui::DestroyContext();

    destroyGLContext();

    if (hwnd) {
        DestroyWindow(hwnd);
        hwnd = nullptr;
    }
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
}

// ---- Scenario list ----

void HubApp::refreshScenarioList() {
    availableScenarios.clear();
    selectedScenarioIdx = -1;

    if (!std::filesystem::exists(scenarioPath))
        return;

    for (const auto& entry : std::filesystem::directory_iterator(scenarioPath)) {
        if (!entry.is_directory()) continue;
        std::string name = entry.path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        // Only show multiplayer scenarios (ending in _mp)
        if (name.size() >= 3 && name.substr(name.size() - 3) == "_mp") {
            availableScenarios.push_back(name);
        }
    }
    std::sort(availableScenarios.begin(), availableScenarios.end());
}

// ---- Start simulation ----

void HubApp::startSimulation() {
    if (selectedScenarioIdx < 0 || selectedScenarioIdx >= (int)availableScenarios.size()) {
        statusMessage = "Please select a scenario first.";
        return;
    }

    if (legacyMode) {
        // Legacy: connect out to peers
        std::string hostnames(hostnameBuf);
        if (hostnames.empty()) {
            statusMessage = "Please enter at least one hostname.";
            return;
        }

        statusMessage = "Connecting to peers...";

        // Save hostname
        if (Utilities::pathExists(userFolder)) {
            std::ofstream file(userFolder + "/hostname-mh.txt");
            if (file.is_open()) {
                file << "hostname=" << hostnames << std::endl;
                file.close();
            }
        }

        network = new Network(port, false);
        network->connectToServer(hostnames);
    }

    // In server mode, network already exists and peers are already connected
    unsigned int connectedPeers = network->getNumberOfConnectedPeers();

    if (connectedPeers == 0) {
        statusMessage = legacyMode ? "Failed to connect to any peers."
                                   : "No players connected yet.";
        if (legacyMode) {
            delete network; network = nullptr;
        }
        return;
    }

    std::string scenarioName = availableScenarios[selectedScenarioIdx];

    // Load scenario
    masterScenarioData = Utilities::getScenarioDataFromFile(scenarioPath + scenarioName, scenarioName);

    // Total ships defined by scenario (each entry in otherShipsData is one player ship)
    totalShips = (uint32_t)masterScenarioData.otherShipsData.size();
    numberOfOtherShips = totalShips > 0 ? totalShips - 1 : 0;

    // Allocate for ALL scenario ships (not just connected peers)
    shipPositions = new ShipPositions(totalShips);
    linesData = new LinesData(totalShips);

    // Initialize peer-to-ship mappings
    shipToPeer.assign(totalShips, -1);
    peerToShip.clear();

    // Time init
    scenarioOffsetTime = Utilities::dmyToTimestamp(masterScenarioData.startDay, masterScenarioData.startMonth, masterScenarioData.startYear);
    scenarioTime = masterScenarioData.startTime * SECONDS_IN_HOUR;
    absoluteTime = Utilities::round(scenarioTime) + scenarioOffsetTime;
    accelerator = 1.0f;
    previousTime = std::chrono::system_clock::now();

    // Assign ships to connected peers and send scenario data
    peerScenarioData.clear();
    numberOfPeers = network->getNumberOfPeers();
    unsigned int nextShip = 0;

    for (unsigned int thisPeer = 0; thisPeer < numberOfPeers; thisPeer++) {
        if (!network->isPeerConnected(thisPeer)) {
            peerToShip.push_back(-1);
            continue;
        }

        if (nextShip >= totalShips) {
            statusMessage = "More peers than ships available in scenario.";
            peerToShip.push_back(-1);
            continue;
        }

        assignShipToPeer(thisPeer, nextShip);
        nextShip++;
    }

    // Drain any pending connection events (they were already handled above)
    network->getNewConnections();
    network->getNewDisconnections();

    unsigned int assignedCount = 0;
    for (unsigned int i = 0; i < totalShips; i++) {
        if (shipToPeer[i] >= 0) assignedCount++;
    }
    statusMessage = "Simulation running: " + std::to_string(assignedCount) + " of " + std::to_string(totalShips) + " ships assigned.";
    phase = RUNNING;
}

void HubApp::assignShipToPeer(unsigned int peerIdx, unsigned int shipIdx) {
    // Ensure peerToShip is large enough
    while (peerToShip.size() <= peerIdx) {
        peerToShip.push_back(-1);
    }

    peerToShip[peerIdx] = (int)shipIdx;
    shipToPeer[shipIdx] = (int)peerIdx;

    // Build per-peer scenario data
    ScenarioData peerData = masterScenarioData;
    peerData.ownShipData.ownShipName = peerData.otherShipsData.at(shipIdx).shipName;
    peerData.ownShipData.initialLat = peerData.otherShipsData.at(shipIdx).initialLat;
    peerData.ownShipData.initialLong = peerData.otherShipsData.at(shipIdx).initialLong;
    if (peerData.otherShipsData.at(shipIdx).legs.size() > 0) {
        peerData.ownShipData.initialSpeed = peerData.otherShipsData.at(shipIdx).legs.at(0).speed;
        peerData.ownShipData.initialBearing = peerData.otherShipsData.at(shipIdx).legs.at(0).bearing;
    } else {
        peerData.ownShipData.initialSpeed = 0;
        peerData.ownShipData.initialBearing = 0;
    }
    peerData.otherShipsData.erase(peerData.otherShipsData.begin() + shipIdx);

    network->sendString(peerData.serialise(false), true, peerIdx);

    // Store for reference
    while (peerScenarioData.size() <= peerIdx) {
        peerScenarioData.push_back(ScenarioData());
    }
    peerScenarioData[peerIdx] = peerData;
}

void HubApp::handleLateJoin(unsigned int peerIdx) {
    // Find next unassigned ship
    int shipIdx = -1;
    for (unsigned int i = 0; i < totalShips; i++) {
        if (shipToPeer[i] == -1) {
            shipIdx = (int)i;
            break;
        }
    }

    if (shipIdx < 0) {
        std::cout << "Player connected but no ships available." << std::endl;
        statusMessage = "Player connected but no ships available.";
        return;
    }

    assignShipToPeer(peerIdx, (unsigned int)shipIdx);

    std::string shipName = (unsigned int)shipIdx < masterScenarioData.otherShipsData.size()
        ? masterScenarioData.otherShipsData[shipIdx].shipName : "Unknown";
    std::cout << "Late join: peer " << peerIdx << " assigned to ship " << shipIdx << " (" << shipName << ")" << std::endl;
    statusMessage = "Player joined as " + shipName;
}

void HubApp::handleDisconnect(unsigned int peerIdx) {
    if (peerIdx < peerToShip.size()) {
        int shipIdx = peerToShip[peerIdx];
        if (shipIdx >= 0 && (unsigned int)shipIdx < totalShips) {
            // Stop the ship (set speed and rate of turn to 0, keep current position/heading)
            float x = 0, z = 0, spd = 0, brg = 0, rot = 0;
            shipPositions->getShipPosition((unsigned int)shipIdx, scenarioTime, x, z, spd, brg, rot);
            shipPositions->setShipPosition((unsigned int)shipIdx, scenarioTime, x, z, 0, brg, 0);

            // Unassign
            shipToPeer[shipIdx] = -1;
            peerToShip[peerIdx] = -1;

            std::string shipName = (unsigned int)shipIdx < masterScenarioData.otherShipsData.size()
                ? masterScenarioData.otherShipsData[shipIdx].shipName : "Unknown";
            std::cout << "Peer " << peerIdx << " disconnected (ship " << shipIdx << " " << shipName << " stopped)" << std::endl;
            statusMessage = shipName + " disconnected - ship stopped.";
        }
    }
}

// ---- Chat handling ----

void HubApp::handleChatMessage(const std::string& msg, unsigned int senderPeer) {
    // Format: CHAT#shipIndex#timestamp#message_text
    std::vector<std::string> parts = Utilities::split(msg, '#');
    if (parts.size() < 4) return;

    ChatMessage chatMsg;
    chatMsg.shipIndex = Utilities::lexical_cast<int>(parts[1]);
    chatMsg.timestamp = parts[2];
    chatMsg.text = parts[3];

    // Look up sender name from scenario data
    if (chatMsg.shipIndex >= 0 && (unsigned int)chatMsg.shipIndex < masterScenarioData.otherShipsData.size()) {
        chatMsg.senderName = masterScenarioData.otherShipsData[chatMsg.shipIndex].shipName;
    } else {
        chatMsg.senderName = "Player " + std::to_string(senderPeer);
    }

    chatMessages.push_back(chatMsg);
    while (chatMessages.size() > MAX_CHAT_MESSAGES) {
        chatMessages.pop_front();
    }

    // Relay to all connected peers (including sender for confirmation)
    for (unsigned int i = 0; i < network->getNumberOfPeers(); i++) {
        if (network->isPeerConnected(i)) {
            network->sendString(msg, true, i);
        }
    }

    std::cout << "[Chat] " << chatMsg.senderName << ": " << chatMsg.text << std::endl;
}

// ---- Simulation update (one tick) ----

void HubApp::updateSimulation() {
    if (!network) return;

    auto currentTime = std::chrono::system_clock::now();
    std::chrono::duration<float> elapsed = currentTime - previousTime;
    previousTime = currentTime;

    float deltaTime = accelerator * std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() / 1000.0f;
    scenarioTime += deltaTime;
    absoluteTime = Utilities::round(scenarioTime) + scenarioOffsetTime;

    std::string timeString = makeTimeString(absoluteTime, scenarioOffsetTime, scenarioTime, accelerator);

    // Listen for messages (handles connects, disconnects, and data)
    network->listenForMessages();

    // Process chat messages
    auto chatMsgs = network->getPendingChatMessages();
    for (auto& cm : chatMsgs) {
        handleChatMessage(cm.second, cm.first);
    }

    // Handle new connections (late join)
    std::vector<unsigned int> newConns = network->getNewConnections();
    for (unsigned int peerIdx : newConns) {
        handleLateJoin(peerIdx);
    }

    // Handle disconnections
    std::vector<unsigned int> newDiscs = network->getNewDisconnections();
    for (unsigned int peerIdx : newDiscs) {
        handleDisconnect(peerIdx);
    }

    // Update peer count
    numberOfPeers = network->getNumberOfPeers();

    // Send updates to each connected peer with an assigned ship
    for (unsigned int thisPeer = 0; thisPeer < numberOfPeers; thisPeer++) {
        if (!network->isPeerConnected(thisPeer)) continue;
        if (thisPeer >= peerToShip.size()) continue;
        int shipIdx = peerToShip[thisPeer];
        if (shipIdx < 0) continue;

        std::string stringToSend = "BC";
        stringToSend += timeString;
        stringToSend += "#";
        stringToSend += "0#"; // Own ship info not used
        stringToSend += Utilities::lexical_cast<std::string>(numberOfOtherShips);
        stringToSend += ",0,0,";
        stringToSend += Utilities::lexical_cast<std::string>(linesData->getNumberOfOtherLines(shipIdx));
        stringToSend += "#";

        // Other ships data (all ships except this peer's own ship)
        std::string otherShipsString;
        for (unsigned int i = 0; i < totalShips; i++) {
            if ((int)i != shipIdx) {
                float x = 0, z = 0, spd = 0, brg = 0, rot = 0;
                shipPositions->getShipPosition(i, scenarioTime, x, z, spd, brg, rot);
                otherShipsString += Utilities::lexical_cast<std::string>(x) + ",";
                otherShipsString += Utilities::lexical_cast<std::string>(z) + ",";
                otherShipsString += Utilities::lexical_cast<std::string>(brg) + ",";
                otherShipsString += Utilities::lexical_cast<std::string>(spd * MPS_TO_KTS) + ",";
                otherShipsString += Utilities::lexical_cast<std::string>(rot) + ",";
                otherShipsString += "0,0,0,0|";
            }
        }
        if (!otherShipsString.empty())
            otherShipsString.pop_back(); // Remove trailing '|'
        stringToSend += otherShipsString + "#";
        stringToSend += "4#5#6#7#8#9#10#";

        std::string linesString = linesData->getLineDataString(shipIdx);
        if (!linesString.empty())
            linesString.pop_back();
        stringToSend += linesString + "#";
        stringToSend += "12";

        network->sendString(stringToSend, false, thisPeer);

        // Process feedback from this peer
        std::string msg = network->getLatestMessage(thisPeer);
        if (msg.length() > 3 && msg.substr(0, 3) == "MPF") {
            msg = msg.substr(3);
            std::vector<std::string> parts = Utilities::split(msg, '#');
            if (parts.size() >= 7) {
                float px = Utilities::lexical_cast<float>(parts[0]);
                float pz = Utilities::lexical_cast<float>(parts[1]);
                float brg = Utilities::lexical_cast<float>(parts[2]);
                float rot = Utilities::lexical_cast<float>(parts[3]);
                float spd = Utilities::lexical_cast<float>(parts[4]);
                float tm = Utilities::lexical_cast<float>(parts[5]);
                shipPositions->setShipPosition((unsigned int)shipIdx, tm, px, pz, spd, brg, rot);

                // Extended state fields (optional, for backward compatibility)
                if (parts.size() >= 12) {
                    float rudder = Utilities::lexical_cast<float>(parts[7]);
                    float rpm = Utilities::lexical_cast<float>(parts[8]);
                    int navLights = Utilities::lexical_cast<int>(parts[9]);
                    int horn = Utilities::lexical_cast<int>(parts[10]);
                    uint32_t mmsi = Utilities::lexical_cast<uint32_t>(parts[11]);
                    shipPositions->setExtendedState((unsigned int)shipIdx, rudder, rpm, navLights, horn, mmsi);
                }

                // Lines data - use ship index for mapping
                std::vector<std::string> linesParts = Utilities::split(parts[6], '|');
                linesData->setLineDataSize((unsigned int)shipIdx, (unsigned int)linesParts.size());
                for (int lineID = 0; lineID < (int)linesParts.size(); lineID++) {
                    std::vector<std::string> ld = Utilities::split(linesParts[lineID], ',');
                    if (ld.size() == 16) {
                        int st = Utilities::lexical_cast<int>(ld[6]);
                        int et = Utilities::lexical_cast<int>(ld[7]);
                        int si = Utilities::lexical_cast<int>(ld[8]);
                        int ei = Utilities::lexical_cast<int>(ld[9]);
                        // Convert local "own ship" to global ship ID
                        if (st == 1) { st = 2; si = shipIdx; }
                        else if (st == 2 && si >= shipIdx) { si++; }
                        if (et == 1) { et = 2; ei = shipIdx; }
                        else if (et == 2 && ei >= shipIdx) { ei++; }
                        linesData->setLineData((unsigned int)shipIdx, lineID,
                            st, et, si, ei,
                            Utilities::lexical_cast<int>(ld[14]),
                            Utilities::lexical_cast<int>(ld[15]),
                            Utilities::lexical_cast<float>(ld[0]),
                            Utilities::lexical_cast<float>(ld[1]),
                            Utilities::lexical_cast<float>(ld[2]),
                            Utilities::lexical_cast<float>(ld[3]),
                            Utilities::lexical_cast<float>(ld[4]),
                            Utilities::lexical_cast<float>(ld[5]),
                            Utilities::lexical_cast<float>(ld[10]),
                            Utilities::lexical_cast<float>(ld[11]),
                            Utilities::lexical_cast<float>(ld[12]),
                            Utilities::lexical_cast<float>(ld[13]));
                    }
                }
            }
        }
    }
}

// ---- Main loop ----

void HubApp::run() {
    MSG msg;
    while (running) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                running = false;
        }
        if (!running) break;

        // Update window size
        RECT rect;
        if (GetClientRect(hwnd, &rect)) {
            windowWidth = rect.right - rect.left;
            windowHeight = rect.bottom - rect.top;
        }

        if (phase == LOBBY && !legacyMode && network) {
            // Server mode lobby: poll for new connections
            network->listenForMessages();
        }

        if (phase == RUNNING) {
            updateSimulation();
            Sleep(sleepTime);
        }

        renderFrame();
    }
}

// ---- ImGui rendering ----

void HubApp::renderFrame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (phase == LOBBY)
        renderLobby();
    else
        renderRunning();

    ImGui::Render();
    glViewport(0, 0, windowWidth, windowHeight);
    glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SwapBuffers(hdc);
}

void HubApp::renderLobby() {
    float w = (float)windowWidth;
    float h = (float)windowHeight;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::Begin("Multiplayer Hub - Lobby", nullptr,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Multiplayer Hub");
    if (legacyMode) {
        ImGui::SameLine();
        ImGui::TextDisabled("(Legacy Mode)");
    }
    ImGui::Separator();
    ImGui::Spacing();

    // Scenario selection
    ImGui::Text("Scenario (must end in _mp):");
    if (ImGui::BeginListBox("##Scenarios", ImVec2(-1, h * 0.25f))) {
        for (int i = 0; i < (int)availableScenarios.size(); i++) {
            bool isSelected = (selectedScenarioIdx == i);
            if (ImGui::Selectable(availableScenarios[i].c_str(), isSelected))
                selectedScenarioIdx = i;
            if (isSelected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndListBox();
    }
    if (availableScenarios.empty()) {
        ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "No _mp scenarios found in %s", scenarioPath.c_str());
    }

    ImGui::Spacing();

    if (legacyMode) {
        // Legacy mode: hostname input
        ImGui::Text("Peer Hostnames (comma-separated):");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##Hostnames", hostnameBuf, sizeof(hostnameBuf));
        ImGui::TextDisabled("e.g. 192.168.1.10,192.168.1.11 or localhost,localhost");
    } else {
        // Server mode: show port and connected players
        ImGui::Text("Server Port: %d", port);

        unsigned int connectedPeers = network ? network->getNumberOfConnectedPeers() : 0;

        ImGui::Spacing();
        ImGui::Text("Connected Players: %u", connectedPeers);
        ImGui::Spacing();

        if (connectedPeers > 0 && ImGui::BeginTable("##Players", 3,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 40);
            ImGui::TableSetupColumn("Address");
            ImGui::TableSetupColumn("Status");
            ImGui::TableHeadersRow();

            for (unsigned int i = 0; i < connectedPeers; i++) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%d", i + 1);
                ImGui::TableSetColumnIndex(1);
                std::string addr = network->getPeerAddress(i);
                ImGui::Text("%s", addr.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextColored(ImVec4(0, 1, 0, 1), "Connected");
            }
            ImGui::EndTable();
        } else if (connectedPeers == 0) {
            ImGui::TextDisabled("Waiting for BC instances to connect to port %d...", port);
        }
    }

    ImGui::Spacing();
    ImGui::Text("Port:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    if (legacyMode) {
        ImGui::InputInt("##Port", &port, 0);
    } else {
        // In server mode, port is read-only after server starts
        ImGui::BeginDisabled();
        ImGui::InputInt("##Port", &port, 0);
        ImGui::EndDisabled();
    }

    ImGui::Spacing();
    ImGui::Spacing();

    // Start button
    bool canStart;
    if (legacyMode) {
        canStart = selectedScenarioIdx >= 0 && strlen(hostnameBuf) > 0;
    } else {
        canStart = selectedScenarioIdx >= 0 && network && network->getNumberOfConnectedPeers() > 0;
    }

    if (!canStart) ImGui::BeginDisabled();
    if (ImGui::Button("Start Simulation", ImVec2(200, 40))) {
        startSimulation();
    }
    if (!canStart) ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Refresh Scenarios", ImVec2(160, 40))) {
        refreshScenarioList();
    }

    // Status message
    if (!statusMessage.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", statusMessage.c_str());
    }

    ImGui::End();
}

void HubApp::renderRunning() {
    float w = (float)windowWidth;
    float h = (float)windowHeight;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::Begin("Multiplayer Hub - Running", nullptr,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    // Header
    std::string timeStr = Utilities::timestampToString(absoluteTime);
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Multiplayer Hub - Running");
    ImGui::SameLine(w - 200);
    ImGui::Text("Time: %s", timeStr.c_str());
    ImGui::Separator();

    // Controls
    ImGui::Spacing();
    if (accelerator > 0) {
        if (ImGui::Button("Pause", ImVec2(80, 30))) accelerator = 0.0f;
    } else {
        if (ImGui::Button("Run", ImVec2(80, 30))) accelerator = 1.0f;
    }
    ImGui::SameLine();
    ImGui::Text("Speed: %.1fx", accelerator);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    ImGui::SliderFloat("##Accel", &accelerator, 0.0f, 10.0f, "%.1f");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Ship status table (shows all scenario ships, not just connected peers)
    if (ImGui::BeginTable("Ships", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Ship", ImGuiTableColumnFlags_WidthFixed, 35);
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Address");
        ImGui::TableSetupColumn("Speed (kts)", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Heading", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Rudder", ImGuiTableColumnFlags_WidthFixed, 55);
        ImGui::TableSetupColumn("RPM", ImGuiTableColumnFlags_WidthFixed, 55);
        ImGui::TableSetupColumn("Status");
        ImGui::TableHeadersRow();

        for (unsigned int i = 0; i < totalShips; i++) {
            float x = 0, z = 0, spd = 0, brg = 0, rot = 0;
            shipPositions->getShipPosition(i, scenarioTime, x, z, spd, brg, rot);

            float rudder = 0, rpm = 0;
            int navL = 0, horn = 0;
            uint32_t mmsi = 0;
            shipPositions->getExtendedState(i, rudder, rpm, navL, horn, mmsi);

            int peerIdx = (i < shipToPeer.size()) ? shipToPeer[i] : -1;
            bool connected = peerIdx >= 0 && network->isPeerConnected((unsigned int)peerIdx);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", i + 1);
            ImGui::TableSetColumnIndex(1);
            if (i < masterScenarioData.otherShipsData.size())
                ImGui::Text("%s", masterScenarioData.otherShipsData[i].shipName.c_str());
            else
                ImGui::Text("-");
            ImGui::TableSetColumnIndex(2);
            if (connected)
                ImGui::Text("%s", network->getPeerAddress((unsigned int)peerIdx).c_str());
            else
                ImGui::Text("-");
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%.1f", spd * MPS_TO_KTS);
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%.0f", brg);
            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%.0f", rudder);
            ImGui::TableSetColumnIndex(6);
            ImGui::Text("%.0f", rpm);
            ImGui::TableSetColumnIndex(7);
            if (connected)
                ImGui::TextColored(ImVec4(0, 1, 0, 1), "Connected");
            else if (peerIdx >= 0)
                ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "Disconnected");
            else
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "Unassigned");
        }

        ImGui::EndTable();
    }

    // Lines info
    if (linesData) {
        int totalLines = linesData->getNumberOfLines();
        if (totalLines > 0) {
            ImGui::Spacing();
            ImGui::Text("Active mooring/towing lines: %d", totalLines);
        }
    }

    // Status
    if (!statusMessage.empty()) {
        ImGui::Spacing();
        ImGui::Text("%s", statusMessage.c_str());
    }

    // Chat section
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Chat");

    float chatHeight = ImGui::GetContentRegionAvail().y - 30;
    if (chatHeight < 60) chatHeight = 60;
    if (ImGui::BeginChild("ChatLog", ImVec2(0, chatHeight), true)) {
        for (const auto& msg : chatMessages) {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "[%s]", msg.timestamp.c_str());
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "%s:", msg.senderName.c_str());
            ImGui::SameLine();
            ImGui::TextWrapped("%s", msg.text.c_str());
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    // Chat input (hub operator can send messages as "Hub")
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 70);
    bool sendChat = ImGui::InputText("##ChatInput", chatInputBuf, sizeof(chatInputBuf),
        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Send", ImVec2(60, 0)) || sendChat) {
        if (strlen(chatInputBuf) > 0 && network) {
            // Build and broadcast chat message from hub
            std::string timeStr = Utilities::timestampToString(absoluteTime);
            std::string chatMsg = "CHAT#-1#" + timeStr + "#" + std::string(chatInputBuf);

            ChatMessage cm;
            cm.shipIndex = -1;
            cm.senderName = "Hub";
            cm.timestamp = timeStr;
            cm.text = chatInputBuf;
            chatMessages.push_back(cm);
            while (chatMessages.size() > MAX_CHAT_MESSAGES)
                chatMessages.pop_front();

            for (unsigned int i = 0; i < network->getNumberOfPeers(); i++) {
                if (network->isPeerConnected(i))
                    network->sendString(chatMsg, true, i);
            }

            chatInputBuf[0] = '\0';
            ImGui::SetKeyboardFocusHere(-1);
        }
    }

    ImGui::End();
}
