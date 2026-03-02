#pragma once

// ImGui-based Multiplayer Hub application.
// Supports two modes:
//   Server mode (default): Hub listens for incoming BC connections
//   Legacy mode (--legacy): Hub connects out to BC peers (original behavior)

#include <string>
#include <vector>
#include <deque>
#include <memory>
#include <chrono>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "../ScenarioDataStructure.hpp"

// Forward declarations
class Network;
class ShipPositions;
class LinesData;

class HubApp {
public:
    bool init(int width, int height, bool legacy = false);
    void run();
    void shutdown();

private:
    // Window and GL context
#ifdef _WIN32
    HWND hwnd = nullptr;
    HDC hdc = nullptr;
    HGLRC hglrc = nullptr;
    WNDCLASSEXW wc = {};
#endif

    bool running = false;
    int windowWidth = 0;
    int windowHeight = 0;

    // Hub phases
    enum Phase { LOBBY, RUNNING };
    Phase phase = LOBBY;

    // Mode
    bool legacyMode = false;

    // Settings from ini
    int port = 18305;
    uint32_t sleepTime = 100;
    std::string userFolder;

    // Lobby state
    std::vector<std::string> availableScenarios;
    int selectedScenarioIdx = -1;
    char hostnameBuf[512] = {};
    std::string scenarioPath;
    std::string statusMessage;

    // Simulation state
    Network* network = nullptr;
    ShipPositions* shipPositions = nullptr;
    LinesData* linesData = nullptr;
    ScenarioData masterScenarioData;
    std::vector<ScenarioData> peerScenarioData;
    unsigned int numberOfPeers = 0;
    uint32_t numberOfOtherShips = 0;
    uint32_t totalShips = 0;

    // Peer-to-ship mapping (peer index -> ship index, -1 if no ship assigned)
    std::vector<int> peerToShip;
    // Ship-to-peer mapping (ship index -> peer index, -1 if unassigned)
    std::vector<int> shipToPeer;

    // Chat
    struct ChatMessage {
        int shipIndex;
        std::string senderName;
        std::string text;
        std::string timestamp;
    };
    std::deque<ChatMessage> chatMessages;
    char chatInputBuf[256] = {};
    static const size_t MAX_CHAT_MESSAGES = 100;
    void handleChatMessage(const std::string& msg, unsigned int senderPeer);

    // Time
    float scenarioTime = 0;
    uint64_t scenarioOffsetTime = 0;
    uint64_t absoluteTime = 0;
    float accelerator = 1.0f;
    std::chrono::time_point<std::chrono::system_clock> previousTime;

    // Platform helpers
    bool createWindow(int width, int height);
    bool createGLContext();
    void destroyGLContext();

    // Lobby
    void refreshScenarioList();
    void startSimulation();

    // Dynamic join/leave
    void handleLateJoin(unsigned int peerIdx);
    void handleDisconnect(unsigned int peerIdx);
    void assignShipToPeer(unsigned int peerIdx, unsigned int shipIdx);

    // Simulation update
    void updateSimulation();

    // ImGui frame rendering
    void renderFrame();
    void renderLobby();
    void renderRunning();

    // Win32 message handler
#ifdef _WIN32
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
#endif
};
