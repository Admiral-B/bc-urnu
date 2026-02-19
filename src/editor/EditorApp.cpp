#include "EditorApp.hpp"
#include "LocationSearch.hpp"
#include "AreaSelector.hpp"
#include "ScenarioFileIO.hpp"
#include "../Utilities.hpp"
#include "../IniFile.hpp"

#ifdef _WIN32
#include <shellapi.h>
#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")
#endif

// ImGui core
#include "../graphics/wicked/imgui/imgui.h"

// ImGui backends
#ifdef _WIN32
#include "../graphics/wicked/imgui/backends/imgui_impl_win32.h"
#include "../graphics/wicked/imgui/backends/imgui_impl_opengl3.h"
#include <GL/gl.h>

// Forward declare WndProc handler from imgui_impl_win32
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
#endif

#include <iostream>
#include <filesystem>
#include <fstream>
#include <thread>
#include <queue>
#include "../miniz/miniz.h"
#include "SatelliteTexture.hpp"
#include "OpenSeaMapSource.hpp"
#include "OSMBuildingReader.hpp"
#include "../BuildingGenerator.hpp"
#include "CoastlineData.hpp"
#include "TileMath.hpp"
#include "../libs/nlohmann/json.hpp"
#include "../libs/stb/stb_image.h"

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

#ifdef WITH_GDAL
#include "../WorldGenerator.hpp"
#endif

// Layout constants
static const float TOOLBAR_HEIGHT = 40.0f;
static const float PROPERTIES_WIDTH = 300.0f;
static const float STATUS_BAR_HEIGHT = 26.0f;
static const float MENU_BAR_HEIGHT = 20.0f;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Approximate degrees-per-pixel at a given zoom level and latitude.
// Based on Web Mercator tile math: at zoom z, the world is 256 * 2^z pixels wide.
static double degreesPerPixelLon(int zoom) {
    return 360.0 / (256.0 * (1 << zoom));
}

static double degreesPerPixelLat(int zoom, double lat) {
    double latRad = lat * M_PI / 180.0;
    return 360.0 / (256.0 * (1 << zoom)) * std::cos(latRad);
    // More accurate: accounts for Mercator stretching
}

// ---- Tile texture helpers ----

unsigned int EditorApp::getOrCreateTileTexture(TileDownloader* dl, int z, int x, int y) {
    char keyBuf[64];
    snprintf(keyBuf, sizeof(keyBuf), "%p/%d/%d/%d", (void*)dl, z, x, y);
    std::string key(keyBuf);

    auto it = tileTexCache.find(key);
    if (it != tileTexCache.end()) {
        return it->second;
    }

    // Try to get PNG data from downloader
    std::vector<uint8_t> pngData = dl->getTile(z, x, y);
    if (pngData.empty()) {
        return 0; // Not ready yet, will be fetched async
    }

    // Decode PNG with stb_image
    int imgW = 0, imgH = 0, channels = 0;
    unsigned char* pixels = stbi_load_from_memory(
        pngData.data(), (int)pngData.size(), &imgW, &imgH, &channels, 4);
    if (!pixels) {
        // Decoding failed - cache a 0 so we don't retry every frame
        tileTexCache[key] = 0;
        texCacheOrder.push_back(key);
        return 0;
    }

    // Upload to OpenGL texture
    unsigned int texId = 0;
    glGenTextures(1, &texId);
    glBindTexture(GL_TEXTURE_2D, texId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, imgW, imgH, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, 0);

    stbi_image_free(pixels);

    tileTexCache[key] = texId;
    texCacheOrder.push_back(key);
    evictOldTextures();

    return texId;
}

void EditorApp::evictOldTextures() {
    while (tileTexCache.size() > MAX_TEX_CACHE && !texCacheOrder.empty()) {
        const std::string& oldest = texCacheOrder.front();
        auto it = tileTexCache.find(oldest);
        if (it != tileTexCache.end()) {
            if (it->second) glDeleteTextures(1, &it->second);
            tileTexCache.erase(it);
        }
        texCacheOrder.erase(texCacheOrder.begin());
    }
}

// ---- Platform: window creation and GL context ----

#ifdef _WIN32

bool EditorApp::createWindow(int width, int height, const std::string& title) {
    wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"BCEditorClass";

    if (!RegisterClassExW(&wc)) {
        std::cerr << "EditorApp: Failed to register window class" << std::endl;
        return false;
    }

    // Convert title to wide string
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, nullptr, 0);
    std::wstring wTitle(wideLen, 0);
    MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, &wTitle[0], wideLen);

    // Adjust window rect for client area size
    RECT wr = { 0, 0, width, height };
    AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, 0);

    hwnd = CreateWindowExW(
        0, wc.lpszClassName, wTitle.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        wr.right - wr.left, wr.bottom - wr.top,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!hwnd) {
        std::cerr << "EditorApp: Failed to create window" << std::endl;
        return false;
    }

    hdc = GetDC(hwnd);
    windowWidth = width;
    windowHeight = height;
    return true;
}

bool EditorApp::createGLContext() {
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
    if (!pixelFormat) {
        std::cerr << "EditorApp: ChoosePixelFormat failed" << std::endl;
        return false;
    }

    if (!SetPixelFormat(hdc, pixelFormat, &pfd)) {
        std::cerr << "EditorApp: SetPixelFormat failed" << std::endl;
        return false;
    }

    hglrc = wglCreateContext(hdc);
    if (!hglrc) {
        std::cerr << "EditorApp: wglCreateContext failed" << std::endl;
        return false;
    }

    if (!wglMakeCurrent(hdc, hglrc)) {
        std::cerr << "EditorApp: wglMakeCurrent failed" << std::endl;
        return false;
    }

    return true;
}

void EditorApp::destroyGLContext() {
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

LRESULT CALLBACK EditorApp::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            // Window resized - will be picked up in render loop
        }
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

bool EditorApp::init(int width, int height, const std::string& title) {
    if (!createWindow(width, height, title))
        return false;

    if (!createGLContext())
        return false;

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // Don't save layout to file for now
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Dark theme
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.FrameRounding = 2.0f;
    style.GrabRounding = 2.0f;
    style.WindowBorderSize = 1.0f;

    // Init backends
#ifdef _WIN32
    ImGui_ImplWin32_Init(hwnd);
#endif
    ImGui_ImplOpenGL3_Init("#version 130");

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // Determine scenario base directory
    std::string userFolder = Utilities::getUserDir();
    if (Utilities::pathExists(userFolder + "Scenarios/")) {
        scenarioBaseDir = userFolder + "Scenarios/";
    } else if (Utilities::pathExists("Scenarios/")) {
        scenarioBaseDir = "Scenarios/";
    } else {
        scenarioBaseDir = "Scenarios/";
    }

    // Scan available ship models
    otherShipModels = ScenarioFileIO::listShipModels("Models/Othership/");
    ownShipModels = ScenarioFileIO::listShipModels("Models/Ownship/");

    // Initialize with a blank scenario
    newScenario();

    // Initialize tile downloaders
    {
        std::string cacheBase;
#ifdef _WIN32
        const char* appdata = std::getenv("APPDATA");
        if (appdata) cacheBase = std::string(appdata) + "/Bridge Command/tilecache";
        else cacheBase = "tilecache";
#else
        cacheBase = "tilecache";
#endif
        satelliteDownloader = std::make_unique<TileDownloader>(
            "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}",
            cacheBase + "/esri/");
        satelliteDownloader->setUserAgent("BridgeCommand/6.0 (scenario-editor)");

        streetDownloader = std::make_unique<TileDownloader>(
            "https://tile.openstreetmap.org/{z}/{x}/{y}.png",
            cacheBase + "/osm/");
        streetDownloader->setUserAgent("BridgeCommand/6.0 (scenario-editor)");

        seamarkDownloader = std::make_unique<TileDownloader>(
            "https://tiles.openseamap.org/seamark/{z}/{x}/{y}.png",
            cacheBase + "/seamark/");
        seamarkDownloader->setUserAgent("BridgeCommand/6.0 (scenario-editor)");
    }

    // Create a 1x1 dark blue placeholder texture for tiles that haven't loaded yet
    {
        unsigned char pixel[4] = { 20, 30, 60, 255 };
        glGenTextures(1, &glPlaceholderTex);
        glBindTexture(GL_TEXTURE_2D, glPlaceholderTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    running = true;
    return true;
}

void EditorApp::shutdown() {
    // Clean up GL tile textures
    for (auto& [key, texId] : tileTexCache) {
        if (texId) glDeleteTextures(1, &texId);
    }
    tileTexCache.clear();
    if (glPlaceholderTex) {
        glDeleteTextures(1, &glPlaceholderTex);
        glPlaceholderTex = 0;
    }

    // Destroy tile downloaders before GL context goes away
    satelliteDownloader.reset();
    streetDownloader.reset();
    seamarkDownloader.reset();

    ImGui_ImplOpenGL3_Shutdown();
#ifdef _WIN32
    ImGui_ImplWin32_Shutdown();
#endif
    ImGui::DestroyContext();

    destroyGLContext();

#ifdef _WIN32
    if (hwnd) {
        DestroyWindow(hwnd);
        hwnd = nullptr;
    }
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
#endif
}

// ---- Main loop ----

void EditorApp::run() {
    MSG msg = {};

    while (running) {
        // Process Win32 messages
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running)
            break;

        // Update window size
        RECT clientRect;
        if (GetClientRect(hwnd, &clientRect)) {
            windowWidth = clientRect.right - clientRect.left;
            windowHeight = clientRect.bottom - clientRect.top;
        }

        if (windowWidth <= 0 || windowHeight <= 0) {
            Sleep(16); // Minimised, don't render
            continue;
        }

        renderFrame();
    }
}

// ---- Frame rendering ----

void EditorApp::renderFrame() {
    // Start ImGui frame
    ImGui_ImplOpenGL3_NewFrame();
#ifdef _WIN32
    ImGui_ImplWin32_NewFrame();
#endif
    ImGui::NewFrame();

    // Keyboard shortcuts
    {
        ImGuiIO& io = ImGui::GetIO();
        bool noTextFocus = !ImGui::GetIO().WantTextInput;

        // General (always active)
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_N))
            newScenario();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O)) {
            refreshScenarioList();
            selectedScenarioIdx = -1;
            showOpenDialog = true;
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S))
            saveScenario();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z))
            undoStack.undo();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y))
            undoStack.redo();
        if (ImGui::IsKeyPressed(ImGuiKey_F5))
            testInSimulator();
        if (ImGui::IsKeyPressed(ImGuiKey_F1))
            showHelpOverlay = !showHelpOverlay;

        // Tool/map shortcuts (only when not typing in a text field)
        if (noTextFocus) {
            // Map navigation
            double panStep = 50.0;
            double dppLon = degreesPerPixelLon(mapZoom);
            double dppLat = degreesPerPixelLat(mapZoom, mapCenterLat);
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
                mapCenterLon -= panStep * dppLon;
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow))
                mapCenterLon += panStep * dppLon;
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
                mapCenterLat += panStep * dppLat;
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
                mapCenterLat -= panStep * dppLat;
            if (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd)) {
                if (mapZoom < 19) mapZoom++;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract)) {
                if (mapZoom > 2) mapZoom--;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Home)) {
                if (scenarioData.ownShipData.initialLat != 0 || scenarioData.ownShipData.initialLong != 0) {
                    mapCenterLat = scenarioData.ownShipData.initialLat;
                    mapCenterLon = scenarioData.ownShipData.initialLong;
                }
            }

            // Tool shortcuts
            if (ImGui::IsKeyPressed(ImGuiKey_S) && !io.KeyCtrl)
                shipPlacer.currentTool = ShipPlacer::SELECT;
            if (ImGui::IsKeyPressed(ImGuiKey_O) && !io.KeyCtrl)
                shipPlacer.currentTool = ShipPlacer::PLACE_OWNSHIP;
            if (ImGui::IsKeyPressed(ImGuiKey_N) && !io.KeyCtrl)
                shipPlacer.currentTool = ShipPlacer::PLACE_OTHERSHIP;
            if (ImGui::IsKeyPressed(ImGuiKey_W))
                shipPlacer.currentTool = ShipPlacer::PLACE_WAYPOINT;
            if (ImGui::IsKeyPressed(ImGuiKey_A) && !io.KeyCtrl)
                areaSelector.startDrawing();
            if (ImGui::IsKeyPressed(ImGuiKey_R)) {
                if (measureTool.getState() != MeasureTool::IDLE)
                    measureTool.clear();
                else
                    measureTool.startMeasuring();
            }
            if (ImGui::IsKeyPressed(ImGuiKey_C) && !io.KeyCtrl)
                loadChartFile();

            // Map tile source toggles
            if (ImGui::IsKeyPressed(ImGuiKey_M))
                tileSource = (tileSource == TILE_SATELLITE) ? TILE_STREET : TILE_SATELLITE;
            if (ImGui::IsKeyPressed(ImGuiKey_K))
                seamarkOverlay = !seamarkOverlay;
            if (ImGui::IsKeyPressed(ImGuiKey_B) && !io.KeyCtrl)
                showBuildings = !showBuildings;

            // Delete selected ship
            if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
                int sel = shipPlacer.getSelectedShip();
                if (sel >= 0 && sel < (int)scenarioData.otherShipsData.size()) {
                    ScenarioData before = scenarioData;
                    scenarioData.otherShipsData.erase(
                        scenarioData.otherShipsData.begin() + sel);
                    shipPlacer.selectShip(-2);
                    ScenarioData after = scenarioData;
                    undoStack.push("Delete ship",
                        [this, before]() { scenarioData = before; shipPlacer.selectShip(-2); scenarioDirty = true; },
                        [this, after]() { scenarioData = after; shipPlacer.selectShip(-2); scenarioDirty = true; });
                    scenarioDirty = true;
                }
            }
        }
    }

    // Full-window background with menu bar
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2((float)windowWidth, (float)windowHeight));
    ImGui::Begin("##MainWindow", nullptr,
        ImGuiWindowFlags_MenuBar |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoBackground);

    renderMenuBar();
    ImGui::End();

    // Get menu bar height for layout
    float menuH = ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.y;
    float topY = menuH;

    // Render fixed-position panels
    renderToolbar(topY);
    topY += TOOLBAR_HEIGHT;

    float mapW = (float)windowWidth - PROPERTIES_WIDTH;
    float mapH = (float)windowHeight - topY - STATUS_BAR_HEIGHT;

    renderMapPanel(0, topY, mapW, mapH);
    renderPropertiesPanel(mapW, topY, PROPERTIES_WIDTH, mapH);
    renderStatusBar(topY + mapH);

    // Modal dialogs
    if (showGenerateDialog)
        renderGenerateWorldDialog();
    renderOpenDialog();
    renderSaveAsDialog();

    // Help overlay
    if (showHelpOverlay)
        renderHelpOverlay();

    // Render
    ImGui::Render();
    glViewport(0, 0, windowWidth, windowHeight);
    glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

#ifdef _WIN32
    SwapBuffers(hdc);
#endif
}

// ---- Panel rendering ----

void EditorApp::renderMenuBar() {
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New Scenario", "Ctrl+N")) {
                newScenario();
            }
            if (ImGui::MenuItem("Open Scenario...", "Ctrl+O")) {
                refreshScenarioList();
                selectedScenarioIdx = -1;
                showOpenDialog = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Save", "Ctrl+S")) {
                saveScenario();
            }
            if (ImGui::MenuItem("Save As...")) {
                strncpy_s(saveAsNameBuf, sizeof(saveAsNameBuf),
                          scenarioData.scenarioName.c_str(), _TRUNCATE);
                showSaveAsDialog = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Export as ZIP...")) {
                exportScenarioZip(false);
            }
            if (ImGui::MenuItem("Export with World...")) {
                exportScenarioZip(true);
            }
            if (ImGui::MenuItem("Import from ZIP...")) {
                importScenarioZip();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4")) {
                running = false;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            std::string undoLabel = undoStack.canUndo()
                ? "Undo " + undoStack.undoDescription() : "Undo";
            std::string redoLabel = undoStack.canRedo()
                ? "Redo " + undoStack.redoDescription() : "Redo";
            if (ImGui::MenuItem(undoLabel.c_str(), "Ctrl+Z", false, undoStack.canUndo())) {
                undoStack.undo();
            }
            if (ImGui::MenuItem(redoLabel.c_str(), "Ctrl+Y", false, undoStack.canRedo())) {
                undoStack.redo();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Satellite Tiles", nullptr, nullptr);
            ImGui::MenuItem("Street Map Tiles", nullptr, nullptr);
            ImGui::MenuItem("Coastlines", nullptr, nullptr);
            ImGui::MenuItem("Grid", nullptr, nullptr);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Scenario")) {
            if (ImGui::MenuItem("Generate World...", nullptr,
                                false, areaSelector.getState() == AreaSelector::COMPLETE)) {
                showGenerateDialog = true;
                // Auto-generate world name from area center
                snprintf(worldNameBuf, sizeof(worldNameBuf), "World_%.2f_%.2f",
                         (areaSelector.getMinLat() + areaSelector.getMaxLat()) / 2.0,
                         (areaSelector.getMinLon() + areaSelector.getMaxLon()) / 2.0);
            }
            if (ImGui::MenuItem("Test in Simulator", "F5")) {
                testInSimulator();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("About")) {}
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
}

void EditorApp::performSearch() {
    std::string query(searchBuf);
    if (query.empty())
        return;

    // First try parsing as coordinates
    double lat, lon;
    if (LocationSearch::parseCoordinates(query, lat, lon)) {
        mapCenterLat = lat;
        mapCenterLon = lon;
        searchResults.clear();
        showSearchResults = false;
        return;
    }

    // Otherwise, search Nominatim
    searchResults = locationSearch.searchPlaceName(query);
    showSearchResults = !searchResults.empty();

    // If exactly one result, jump directly
    if (searchResults.size() == 1) {
        mapCenterLat = searchResults[0].lat;
        mapCenterLon = searchResults[0].lon;
        showSearchResults = false;
    }
}

void EditorApp::renderToolbar(float topY) {
    ImGui::SetNextWindowPos(ImVec2(0, topY));
    ImGui::SetNextWindowSize(ImVec2((float)windowWidth, TOOLBAR_HEIGHT));

    ImGui::Begin("##Toolbar", nullptr,
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse);

    {
        bool isSelect = (shipPlacer.currentTool == ShipPlacer::SELECT);
        if (isSelect) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
        if (ImGui::Button("Select")) {
            shipPlacer.currentTool = ShipPlacer::SELECT;
        }
        if (isSelect) ImGui::PopStyleColor();
    }
    ImGui::SameLine();

    if (ImGui::Button("Own Ship")) {
        shipPlacer.currentTool = ShipPlacer::PLACE_OWNSHIP;
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Ship")) {
        shipPlacer.currentTool = ShipPlacer::PLACE_OTHERSHIP;
    }
    ImGui::SameLine();

    {
        bool isDrawing = (areaSelector.getState() == AreaSelector::DRAWING);
        bool isComplete = (areaSelector.getState() == AreaSelector::COMPLETE);

        if (isDrawing) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.6f, 0.0f, 1.0f));
            if (ImGui::Button("Drawing..."))
                areaSelector.clear();
            ImGui::PopStyleColor();
        } else if (isComplete) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.6f, 0.0f, 1.0f));
            ImGui::Button("Area Set");
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::Button("Clear"))
                areaSelector.clear();
        } else {
            if (ImGui::Button("Draw Area"))
                areaSelector.startDrawing();
        }
    }

    ImGui::SameLine();
    {
        bool isMeasuring = (measureTool.getState() != MeasureTool::IDLE);
        if (isMeasuring)
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
        if (ImGui::Button("Measure")) {
            if (isMeasuring)
                measureTool.clear();
            else
                measureTool.startMeasuring();
        }
        if (isMeasuring)
            ImGui::PopStyleColor();
    }
    ImGui::SameLine();

    // Chart overlay button
    if (chartOverlay.isLoaded()) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.5f, 0.3f, 1.0f));
        char chartLabel[32];
        snprintf(chartLabel, sizeof(chartLabel), "Chart (%d)", chartOverlay.chartCount());
        if (ImGui::Button(chartLabel)) {
            chartOverlay.clear();
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button("+Chart")) {
            loadChartFile();
        }
    } else {
        if (ImGui::Button("Load Chart")) {
            loadChartFile();
        }
    }
    ImGui::SameLine();

    ImGui::Text("|");
    ImGui::SameLine();

    // Search / Go-to location
    ImGui::SetNextItemWidth(250.0f);
    bool enterPressed = ImGui::InputTextWithHint("##search", "Go to location (coords or name)...",
        searchBuf, sizeof(searchBuf),
        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Go") || enterPressed) {
        performSearch();
    }

    // Show search results popup
    if (showSearchResults && !searchResults.empty()) {
        ImGui::SameLine();
        ImGui::Text("Results:");
        // Use a child window for results below the toolbar
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMin().x - 200.0f, topY + TOOLBAR_HEIGHT));
        ImGui::SetNextWindowSize(ImVec2(450.0f, 0.0f)); // auto height
        ImGui::Begin("##SearchResults", &showSearchResults,
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings);

        for (size_t i = 0; i < searchResults.size(); i++) {
            const auto& r = searchResults[i];
            char label[256];
            snprintf(label, sizeof(label), "%.4f, %.4f - %s##%zu",
                     r.lat, r.lon, r.displayName.c_str(), i);
            if (ImGui::Selectable(label)) {
                mapCenterLat = r.lat;
                mapCenterLon = r.lon;
                showSearchResults = false;
            }
        }

        ImGui::End();
    }

    ImGui::End();
}

// Convert lat/lon to pixel position within the map panel
static ImVec2 latLonToPixel(double lat, double lon,
                             double centerLat, double centerLon, int zoom,
                             float panelCx, float panelCy) {
    double dppLon = degreesPerPixelLon(zoom);
    double dppLat = degreesPerPixelLat(zoom, centerLat);
    float px = panelCx + (float)((lon - centerLon) / dppLon);
    float py = panelCy - (float)((lat - centerLat) / dppLat); // Y is inverted
    return ImVec2(px, py);
}

// Convert pixel position to lat/lon
static void pixelToLatLon(float px, float py,
                           double centerLat, double centerLon, int zoom,
                           float panelCx, float panelCy,
                           double& outLat, double& outLon) {
    double dppLon = degreesPerPixelLon(zoom);
    double dppLat = degreesPerPixelLat(zoom, centerLat);
    outLon = centerLon + (px - panelCx) * dppLon;
    outLat = centerLat - (py - panelCy) * dppLat; // Y is inverted
}

void EditorApp::renderMapPanel(float x, float y, float w, float h) {
    ImGui::SetNextWindowPos(ImVec2(x, y));
    ImGui::SetNextWindowSize(ImVec2(w, h));

    ImGui::Begin("Map", nullptr,
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse);

    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 cursorPos = ImGui::GetCursorScreenPos();

    // Map content area
    float mapX = cursorPos.x;
    float mapY = cursorPos.y;
    float mapW = avail.x;
    float mapH = avail.y;
    float panelCx = mapX + mapW * 0.5f;
    float panelCy = mapY + mapH * 0.5f;

    // Clip rendering to map area
    drawList->PushClipRect(ImVec2(mapX, mapY), ImVec2(mapX + mapW, mapY + mapH), true);

    // Dark blue background (shows through where tiles haven't loaded)
    drawList->AddRectFilled(
        cursorPos,
        ImVec2(mapX + mapW, mapY + mapH),
        IM_COL32(20, 30, 60, 255));

    // Render map tiles
    {
        TileDownloader* dl = (tileSource == TILE_SATELLITE)
            ? satelliteDownloader.get() : streetDownloader.get();
        if (dl) {
            // Calculate the global pixel position of the map center
            double centerPixelX = TileMath::lonToPixelX(mapCenterLon, mapZoom);
            double centerPixelY = TileMath::latToPixelY(mapCenterLat, mapZoom);

            // Calculate visible tile range
            double leftPixel = centerPixelX - mapW * 0.5;
            double rightPixel = centerPixelX + mapW * 0.5;
            double topPixel = centerPixelY - mapH * 0.5;
            double bottomPixel = centerPixelY + mapH * 0.5;

            int tileMinX = (int)std::floor(leftPixel / 256.0);
            int tileMaxX = (int)std::floor(rightPixel / 256.0);
            int tileMinY = (int)std::floor(topPixel / 256.0);
            int tileMaxY = (int)std::floor(bottomPixel / 256.0);

            int maxTile = (1 << mapZoom) - 1;
            tileMinX = std::max(0, tileMinX);
            tileMaxX = std::min(maxTile, tileMaxX);
            tileMinY = std::max(0, tileMinY);
            tileMaxY = std::min(maxTile, tileMaxY);

            for (int ty = tileMinY; ty <= tileMaxY; ty++) {
                for (int tx = tileMinX; tx <= tileMaxX; tx++) {
                    // Calculate screen position of this tile
                    float screenX = mapX + (float)(tx * 256.0 - leftPixel);
                    float screenY = mapY + (float)(ty * 256.0 - topPixel);

                    unsigned int texId = getOrCreateTileTexture(dl, mapZoom, tx, ty);
                    if (texId) {
                        drawList->AddImage(
                            (ImTextureID)(intptr_t)texId,
                            ImVec2(screenX, screenY),
                            ImVec2(screenX + 256.0f, screenY + 256.0f));
                    }
                }
            }

            // Render OpenSeaMap seamark overlay on top
            if (seamarkOverlay && seamarkDownloader) {
                for (int ty = tileMinY; ty <= tileMaxY; ty++) {
                    for (int tx = tileMinX; tx <= tileMaxX; tx++) {
                        float screenX = mapX + (float)(tx * 256.0 - leftPixel);
                        float screenY = mapY + (float)(ty * 256.0 - topPixel);

                        unsigned int texId = getOrCreateTileTexture(seamarkDownloader.get(), mapZoom, tx, ty);
                        if (texId) {
                            drawList->AddImage(
                                (ImTextureID)(intptr_t)texId,
                                ImVec2(screenX, screenY),
                                ImVec2(screenX + 256.0f, screenY + 256.0f));
                        }
                    }
                }
            }
        }
    }

    drawList->PopClipRect();

    // Handle mouse interaction in map area
    ImGui::InvisibleButton("##MapArea", avail);
    bool isMapHovered = ImGui::IsItemHovered();

    if (isMapHovered) {
        ImVec2 mousePos = ImGui::GetMousePos();
        pixelToLatLon(mousePos.x, mousePos.y,
                      mapCenterLat, mapCenterLon, mapZoom,
                      panelCx, panelCy, mouseLat, mouseLon);

        // Area selector: update preview while drawing
        if (areaSelector.getState() == AreaSelector::DRAWING) {
            areaSelector.updatePreview(mouseLat, mouseLon);
        }

        // Measure tool: update preview while placing B
        if (measureTool.getState() == MeasureTool::PLACING_B) {
            measureTool.updatePreview(mouseLat, mouseLon);
        }

        // Hover tooltip for ships
        {
            float hoverRadius = 15.0f;
            int hoverShip = -2;
            float hoverDist = hoverRadius;
            ImVec2 mPos = ImGui::GetMousePos();

            if (scenarioData.ownShipData.initialLat != 0 || scenarioData.ownShipData.initialLong != 0) {
                ImVec2 sp = latLonToPixel(scenarioData.ownShipData.initialLat,
                                           scenarioData.ownShipData.initialLong,
                                           mapCenterLat, mapCenterLon, mapZoom, panelCx, panelCy);
                float dx = mPos.x - sp.x, dy = mPos.y - sp.y;
                float dist = std::sqrt(dx * dx + dy * dy);
                if (dist < hoverDist) { hoverDist = dist; hoverShip = -1; }
            }
            for (int i = 0; i < (int)scenarioData.otherShipsData.size(); i++) {
                const auto& ship = scenarioData.otherShipsData[i];
                ImVec2 sp = latLonToPixel(ship.initialLat, ship.initialLong,
                                           mapCenterLat, mapCenterLon, mapZoom, panelCx, panelCy);
                float dx = mPos.x - sp.x, dy = mPos.y - sp.y;
                float dist = std::sqrt(dx * dx + dy * dy);
                if (dist < hoverDist) { hoverDist = dist; hoverShip = i; }
            }

            if (hoverShip >= -1) {
                ImGui::BeginTooltip();
                if (hoverShip == -1) {
                    const auto& own = scenarioData.ownShipData;
                    ImGui::Text("Own Ship: %s", own.ownShipName.empty() ? "(unnamed)" : own.ownShipName.c_str());
                    ImGui::Text("Course: %.0f deg  Speed: %.1f kts", own.initialBearing, own.initialSpeed);
                    ImGui::Text("Pos: %.4f, %.4f", own.initialLat, own.initialLong);
                } else {
                    const auto& ship = scenarioData.otherShipsData[hoverShip];
                    ImGui::Text("Ship %d: %s", hoverShip + 1, ship.shipName.c_str());
                    ImGui::Text("Legs: %d", (int)ship.legs.size());
                    ImGui::Text("Pos: %.4f, %.4f", ship.initialLat, ship.initialLong);
                    if (ship.mmsi > 0) ImGui::Text("MMSI: %u", ship.mmsi);
                }
                ImGui::EndTooltip();
            }
        }

        // Handle left click (skip if this is a double-click frame to avoid
        // accidentally placing waypoints when the user double-clicks to pan)
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            !ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            if (measureTool.getState() == MeasureTool::PLACING_A ||
                measureTool.getState() == MeasureTool::PLACING_B) {
                measureTool.handleClick(mouseLat, mouseLon);
            } else if (areaSelector.getState() == AreaSelector::DRAWING) {
                areaSelector.handleClick(mouseLat, mouseLon);
            } else if (shipPlacer.currentTool != ShipPlacer::SELECT) {
                // Snapshot before for undo
                ScenarioData before = scenarioData;
                int selBefore = shipPlacer.getSelectedShip();
                auto toolBefore = shipPlacer.currentTool;
                if (shipPlacer.handleMapClick(mouseLat, mouseLon, scenarioData)) {
                    ScenarioData after = scenarioData;
                    int selAfter = shipPlacer.getSelectedShip();
                    auto toolAfter = shipPlacer.currentTool;
                    std::string desc;
                    if (toolBefore == ShipPlacer::PLACE_OWNSHIP) desc = "Place own ship";
                    else if (toolBefore == ShipPlacer::PLACE_OTHERSHIP) desc = "Add ship";
                    else if (toolBefore == ShipPlacer::PLACE_WAYPOINT) desc = "Add waypoint";
                    else desc = "Ship edit";
                    undoStack.push(desc,
                        [this, before, selBefore]() {
                            scenarioData = before;
                            shipPlacer.selectShip(selBefore);
                            scenarioDirty = true;
                        },
                        [this, after, selAfter, toolAfter]() {
                            scenarioData = after;
                            shipPlacer.selectShip(selAfter);
                            shipPlacer.currentTool = toolAfter;
                            scenarioDirty = true;
                        });
                    scenarioDirty = true;
                }
            } else {
                // SELECT mode: hit-test ships
                ImVec2 clickPos = ImGui::GetMousePos();
                float hitRadius = 15.0f;
                int bestShip = -2; // -2 = nothing
                float bestDist = hitRadius;

                // Test own ship
                if (scenarioData.ownShipData.initialLat != 0 || scenarioData.ownShipData.initialLong != 0) {
                    ImVec2 sp = latLonToPixel(scenarioData.ownShipData.initialLat,
                                               scenarioData.ownShipData.initialLong,
                                               mapCenterLat, mapCenterLon, mapZoom, panelCx, panelCy);
                    float dx = clickPos.x - sp.x, dy = clickPos.y - sp.y;
                    float dist = std::sqrt(dx * dx + dy * dy);
                    if (dist < bestDist) { bestDist = dist; bestShip = -1; }
                }

                // Test other ships
                for (int i = 0; i < (int)scenarioData.otherShipsData.size(); i++) {
                    const auto& ship = scenarioData.otherShipsData[i];
                    ImVec2 sp = latLonToPixel(ship.initialLat, ship.initialLong,
                                               mapCenterLat, mapCenterLon, mapZoom, panelCx, panelCy);
                    float dx = clickPos.x - sp.x, dy = clickPos.y - sp.y;
                    float dist = std::sqrt(dx * dx + dy * dy);
                    if (dist < bestDist) { bestDist = dist; bestShip = i; }
                }

                shipPlacer.selectShip(bestShip);
            }
        }

        // Escape cancels current tool
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            shipPlacer.currentTool = ShipPlacer::SELECT;
            if (areaSelector.getState() == AreaSelector::DRAWING)
                areaSelector.clear();
            if (measureTool.getState() != MeasureTool::IDLE)
                measureTool.clear();
        }

        // Left-drag to move selected ship (SELECT mode only)
        if (shipPlacer.currentTool == ShipPlacer::SELECT &&
            ImGui::IsMouseDragging(ImGuiMouseButton_Left, 5.0f)) {
            int sel = shipPlacer.getSelectedShip();
            if (sel >= -1) {
                if (!isDraggingShip) {
                    isDraggingShip = true;
                    dragBeforeState = scenarioData;
                }
                if (sel == -1) {
                    scenarioData.ownShipData.initialLat = (float)mouseLat;
                    scenarioData.ownShipData.initialLong = (float)mouseLon;
                } else if (sel < (int)scenarioData.otherShipsData.size()) {
                    scenarioData.otherShipsData[sel].initialLat = (float)mouseLat;
                    scenarioData.otherShipsData[sel].initialLong = (float)mouseLon;
                }
                scenarioDirty = true;
            }
        }
        if (isDraggingShip && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            isDraggingShip = false;
            ScenarioData before = dragBeforeState;
            ScenarioData after = scenarioData;
            undoStack.push("Move ship",
                [this, before]() { scenarioData = before; scenarioDirty = true; },
                [this, after]() { scenarioData = after; scenarioDirty = true; });
        }

        // Scroll wheel zoom (centred on mouse position)
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0) {
            int newZoom = mapZoom + (wheel > 0 ? 1 : -1);
            newZoom = std::max(2, std::min(19, newZoom));
            if (newZoom != mapZoom) {
                // Shift map center so the point under the cursor stays fixed
                double dppLonOld = degreesPerPixelLon(mapZoom);
                double dppLatOld = degreesPerPixelLat(mapZoom, mapCenterLat);
                double dppLonNew = degreesPerPixelLon(newZoom);
                double dppLatNew = degreesPerPixelLat(newZoom, mapCenterLat);
                ImVec2 mousePos = ImGui::GetMousePos();
                double offsetX = mousePos.x - panelCx;
                double offsetY = mousePos.y - panelCy;
                mapCenterLon += offsetX * (dppLonOld - dppLonNew);
                mapCenterLat -= offsetY * (dppLatOld - dppLatNew);
                mapZoom = newZoom;
            }
        }

        // Double-click to centre map on that location
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            mapCenterLat = mouseLat;
            mapCenterLon = mouseLon;
        }

        // Right-click drag to pan
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
            ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Right);
            ImGui::ResetMouseDragDelta(ImGuiMouseButton_Right);
            double dppLon = degreesPerPixelLon(mapZoom);
            double dppLat = degreesPerPixelLat(mapZoom, mapCenterLat);
            mapCenterLon -= delta.x * dppLon;
            mapCenterLat += delta.y * dppLat;
        }

        // Right-click context menu (only if no drag occurred)
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
            ImVec2 clickStart = ImGui::GetIO().MouseClickedPos[1];
            ImVec2 mPos = ImGui::GetMousePos();
            float ddx = mPos.x - clickStart.x, ddy = mPos.y - clickStart.y;
            if (ddx * ddx + ddy * ddy < 25.0f) {
                contextMenuLat = mouseLat;
                contextMenuLon = mouseLon;
                contextMenuHitShip = -2;
                contextMenuHitLeg = -1;
                contextMenuLegShip = -1;

                float hitRadius = 15.0f;
                float bestDist = hitRadius;

                // Hit-test own ship
                if (scenarioData.ownShipData.initialLat != 0 || scenarioData.ownShipData.initialLong != 0) {
                    ImVec2 sp = latLonToPixel(scenarioData.ownShipData.initialLat,
                                               scenarioData.ownShipData.initialLong,
                                               mapCenterLat, mapCenterLon, mapZoom, panelCx, panelCy);
                    float dx = mPos.x - sp.x, dy = mPos.y - sp.y;
                    float dist = std::sqrt(dx * dx + dy * dy);
                    if (dist < bestDist) { bestDist = dist; contextMenuHitShip = -1; }
                }

                // Hit-test other ships
                for (int i = 0; i < (int)scenarioData.otherShipsData.size(); i++) {
                    const auto& ship = scenarioData.otherShipsData[i];
                    ImVec2 sp = latLonToPixel(ship.initialLat, ship.initialLong,
                                               mapCenterLat, mapCenterLon, mapZoom, panelCx, panelCy);
                    float dx = mPos.x - sp.x, dy = mPos.y - sp.y;
                    float dist = std::sqrt(dx * dx + dy * dy);
                    if (dist < bestDist) { bestDist = dist; contextMenuHitShip = i; contextMenuHitLeg = -1; }
                }

                // Hit-test waypoints (if no ship was hit closer)
                for (int i = 0; i < (int)scenarioData.otherShipsData.size(); i++) {
                    const auto& ship = scenarioData.otherShipsData[i];
                    float curLat = ship.initialLat;
                    float curLon = ship.initialLong;
                    for (int j = 0; j < (int)ship.legs.size(); j++) {
                        const auto& leg = ship.legs[j];
                        if (leg.speed == 0 && leg.distance == 0) continue;
                        float bearRad = leg.bearing * (float)M_PI / 180.0f;
                        float distDeg = leg.distance / 60.0f;
                        float nextLat = curLat + distDeg * std::cos(bearRad);
                        float nextLon = curLon + distDeg * std::sin(bearRad) /
                                        std::cos(curLat * (float)M_PI / 180.0f);
                        ImVec2 wp = latLonToPixel(nextLat, nextLon,
                                                   mapCenterLat, mapCenterLon, mapZoom, panelCx, panelCy);
                        float dx = mPos.x - wp.x, dy = mPos.y - wp.y;
                        float dist = std::sqrt(dx * dx + dy * dy);
                        if (dist < bestDist) {
                            bestDist = dist;
                            contextMenuHitShip = -2;
                            contextMenuHitLeg = j;
                            contextMenuLegShip = i;
                        }
                        curLat = nextLat;
                        curLon = nextLon;
                    }
                }

                ImGui::OpenPopup("##MapContextMenu");
            }
        }
    }

    // Render area selector rectangle
    if (areaSelector.getState() != AreaSelector::IDLE) {
        ImVec2 p1 = latLonToPixel(areaSelector.getMinLat(), areaSelector.getMinLon(),
                                    mapCenterLat, mapCenterLon, mapZoom, panelCx, panelCy);
        ImVec2 p2 = latLonToPixel(areaSelector.getMaxLat(), areaSelector.getMaxLon(),
                                    mapCenterLat, mapCenterLon, mapZoom, panelCx, panelCy);

        // Swap so p1 is top-left, p2 is bottom-right in screen coords
        float left = std::min(p1.x, p2.x);
        float right = std::max(p1.x, p2.x);
        float top = std::min(p1.y, p2.y);
        float bottom = std::max(p1.y, p2.y);

        // Semi-transparent fill
        drawList->AddRectFilled(
            ImVec2(left, top), ImVec2(right, bottom),
            IM_COL32(255, 200, 50, 40));

        // Border
        ImU32 borderColor = (areaSelector.getState() == AreaSelector::COMPLETE)
            ? IM_COL32(50, 255, 50, 200)   // Green when complete
            : IM_COL32(255, 200, 50, 200);  // Yellow while drawing
        drawList->AddRect(
            ImVec2(left, top), ImVec2(right, bottom),
            borderColor, 0.0f, 0, 2.0f);

        // Corner handles
        float handleSize = 4.0f;
        drawList->AddRectFilled(ImVec2(left - handleSize, top - handleSize),
                                ImVec2(left + handleSize, top + handleSize), borderColor);
        drawList->AddRectFilled(ImVec2(right - handleSize, top - handleSize),
                                ImVec2(right + handleSize, top + handleSize), borderColor);
        drawList->AddRectFilled(ImVec2(left - handleSize, bottom - handleSize),
                                ImVec2(left + handleSize, bottom + handleSize), borderColor);
        drawList->AddRectFilled(ImVec2(right - handleSize, bottom - handleSize),
                                ImVec2(right + handleSize, bottom + handleSize), borderColor);

        // Dimensions label
        if (areaSelector.getState() == AreaSelector::COMPLETE ||
            (areaSelector.getState() == AreaSelector::DRAWING &&
             (areaSelector.getMinLat() != areaSelector.getMaxLat()))) {
            char dimText[128];
            snprintf(dimText, sizeof(dimText), "%.1f km x %.1f km",
                     areaSelector.getWidthKm(), areaSelector.getHeightKm());
            ImVec2 dimSize = ImGui::CalcTextSize(dimText);
            float labelX = (left + right - dimSize.x) * 0.5f;
            float labelY = bottom + 4.0f;
            drawList->AddRectFilled(ImVec2(labelX - 2, labelY - 1),
                                    ImVec2(labelX + dimSize.x + 2, labelY + dimSize.y + 1),
                                    IM_COL32(0, 0, 0, 180));
            drawList->AddText(ImVec2(labelX, labelY), IM_COL32(255, 255, 255, 255), dimText);
        }
    }

    // Render S-57 chart overlay (if loaded)
    if (chartOverlay.isLoaded()) {
        auto chartToPixel = [](double lat, double lon, void* ud) -> ImVec2 {
            struct Ctx { double cLat, cLon; int zoom; float cx, cy; };
            auto* c = static_cast<Ctx*>(ud);
            return latLonToPixel(lat, lon, c->cLat, c->cLon, c->zoom, c->cx, c->cy);
        };
        struct { double cLat, cLon; int zoom; float cx, cy; }
            chartCtx = { mapCenterLat, mapCenterLon, mapZoom, panelCx, panelCy };
        chartOverlay.render(drawList, mapZoom, chartToPixel, &chartCtx,
                            mapX, mapY, mapW, mapH);
    }

    // Render buoys and lights (only at zoom >= 10)
    if (mapZoom >= 10) {
        // Buoys: small diamonds
        for (const auto& buoy : buoyData) {
            ImVec2 pos = latLonToPixel(buoy.lat, buoy.lon,
                                        mapCenterLat, mapCenterLon, mapZoom, panelCx, panelCy);
            // Only draw if on screen
            if (pos.x >= mapX - 10 && pos.x <= mapX + mapW + 10 &&
                pos.y >= mapY - 10 && pos.y <= mapY + mapH + 10) {
                float s = 4.0f;
                drawList->AddQuadFilled(
                    ImVec2(pos.x, pos.y - s), ImVec2(pos.x + s, pos.y),
                    ImVec2(pos.x, pos.y + s), ImVec2(pos.x - s, pos.y),
                    IM_COL32(255, 200, 0, 200));
                drawList->AddQuad(
                    ImVec2(pos.x, pos.y - s), ImVec2(pos.x + s, pos.y),
                    ImVec2(pos.x, pos.y + s), ImVec2(pos.x - s, pos.y),
                    IM_COL32(0, 0, 0, 200), 1.0f);
                // Show type name at higher zoom
                if (mapZoom >= 14 && !buoy.typeName.empty()) {
                    drawList->AddText(ImVec2(pos.x + s + 2, pos.y - 6),
                                      IM_COL32(255, 200, 0, 180), buoy.typeName.c_str());
                }
            }
        }

        // Lights: small colored circles with glow
        for (const auto& light : lightData) {
            // Skip lights attached to buoys (they share position with the buoy)
            // but still draw a glow ring at higher zoom
            ImVec2 pos = latLonToPixel(light.lat, light.lon,
                                        mapCenterLat, mapCenterLon, mapZoom, panelCx, panelCy);
            if (pos.x >= mapX - 10 && pos.x <= mapX + mapW + 10 &&
                pos.y >= mapY - 10 && pos.y <= mapY + mapH + 10) {
                ImU32 col = IM_COL32(light.r, light.g, light.b, 220);
                ImU32 glowCol = IM_COL32(light.r, light.g, light.b, 60);
                float r = 3.0f;
                // Glow ring proportional to range
                float glowR = r + std::min(light.range * 0.5f, 12.0f);
                drawList->AddCircleFilled(pos, glowR, glowCol, 12);
                drawList->AddCircleFilled(pos, r, col, 8);
                // Show range at higher zoom
                if (mapZoom >= 14) {
                    char rangeLabel[32];
                    snprintf(rangeLabel, sizeof(rangeLabel), "%.0fNM", light.range);
                    drawList->AddText(ImVec2(pos.x + r + 2, pos.y - 6),
                                      IM_COL32(light.r, light.g, light.b, 160), rangeLabel);
                }
            }
        }
    }

    // Render building footprints (OSM)
    if (showBuildings && mapZoom >= 14) {
        // Query Overpass API if we haven't already for this view area
        // Re-query when center moves significantly or zoom changes
        double dLat = std::abs(mapCenterLat - buildingsQueryLat);
        double dLon = std::abs(mapCenterLon - buildingsQueryLon);
        bool needQuery = !buildingsQueried || dLat > 0.02 || dLon > 0.02 ||
                         std::abs(mapZoom - buildingsQueryZoom) > 2;

        if (needQuery) {
            // Compute visible bounds
            double dppLon = degreesPerPixelLon(mapZoom);
            double dppLat = degreesPerPixelLat(mapZoom, mapCenterLat);
            double halfW = (mapW * 0.5) * dppLon;
            double halfH = (mapH * 0.5) * dppLat;
            double qMinLat = mapCenterLat - halfH;
            double qMaxLat = mapCenterLat + halfH;
            double qMinLon = mapCenterLon - halfW;
            double qMaxLon = mapCenterLon + halfW;

            buildingReader.query(qMinLat, qMaxLat, qMinLon, qMaxLon);
            buildingsQueried = true;
            buildingsQueryLat = mapCenterLat;
            buildingsQueryLon = mapCenterLon;
            buildingsQueryZoom = mapZoom;
        }

        // Draw building footprints as filled polygons
        ImU32 fillCol = IM_COL32(180, 140, 100, 80);
        ImU32 outlineCol = IM_COL32(160, 120, 80, 180);
        float thickness = (mapZoom >= 16) ? 1.5f : 1.0f;

        for (const auto& bldg : buildingReader.getBuildings()) {
            if (bldg.outline.size() < 3) continue;

            // Convert to screen coords and cull
            std::vector<ImVec2> screenPoly;
            screenPoly.reserve(bldg.outline.size());
            bool anyOnScreen = false;

            for (const auto& [lat, lon] : bldg.outline) {
                ImVec2 p = latLonToPixel(lat, lon, mapCenterLat, mapCenterLon,
                                          mapZoom, panelCx, panelCy);
                screenPoly.push_back(p);
                if (p.x >= mapX - 20 && p.x <= mapX + mapW + 20 &&
                    p.y >= mapY - 20 && p.y <= mapY + mapH + 20)
                    anyOnScreen = true;
            }

            if (!anyOnScreen) continue;

            // Fill (convex approximation -- fine for most buildings)
            if (screenPoly.size() >= 3 && screenPoly.size() <= 64) {
                drawList->AddConvexPolyFilled(screenPoly.data(),
                                               static_cast<int>(screenPoly.size()), fillCol);
            }

            // Outline
            for (size_t i = 0; i < screenPoly.size(); i++) {
                size_t j = (i + 1) % screenPoly.size();
                drawList->AddLine(screenPoly[i], screenPoly[j], outlineCol, thickness);
            }

            // Show building name at high zoom
            if (mapZoom >= 17 && !bldg.name.empty()) {
                ImVec2 ctr = screenPoly[0];
                for (size_t i = 1; i < screenPoly.size(); i++) {
                    ctr.x += screenPoly[i].x;
                    ctr.y += screenPoly[i].y;
                }
                ctr.x /= screenPoly.size();
                ctr.y /= screenPoly.size();
                ImVec2 textSize = ImGui::CalcTextSize(bldg.name.c_str());
                drawList->AddText(ImVec2(ctr.x - textSize.x * 0.5f, ctr.y - textSize.y * 0.5f),
                                  IM_COL32(80, 60, 40, 200), bldg.name.c_str());
            }
        }
    }

    // Render ships
    {
        // Package the conversion parameters for the callback
        struct MapContext {
            double centerLat, centerLon;
            int zoom;
            float panelCx, panelCy;
        };
        MapContext ctx = { mapCenterLat, mapCenterLon, mapZoom, panelCx, panelCy };

        auto toPixelFn = [](double lat, double lon, void* ud) -> ImVec2 {
            auto* c = static_cast<MapContext*>(ud);
            return latLonToPixel(lat, lon, c->centerLat, c->centerLon,
                                 c->zoom, c->panelCx, c->panelCy);
        };

        shipPlacer.renderShips(drawList, scenarioData, toPixelFn, &ctx);
    }

    // Render measurement line
    if (measureTool.getState() != MeasureTool::IDLE) {
        auto measureToPixel = [](double lat, double lon, void* ud) -> ImVec2 {
            struct Ctx { double cLat, cLon; int zoom; float cx, cy; };
            auto* c = static_cast<Ctx*>(ud);
            return latLonToPixel(lat, lon, c->cLat, c->cLon, c->zoom, c->cx, c->cy);
        };
        struct { double cLat, cLon; int zoom; float cx, cy; }
            mCtx = { mapCenterLat, mapCenterLon, mapZoom, panelCx, panelCy };
        measureTool.render(drawList, measureToPixel, &mCtx, measureTool.measureSpeed);
    }

    // Right-click context menu popup
    if (ImGui::BeginPopup("##MapContextMenu")) {
        if (contextMenuHitLeg >= 0 && contextMenuLegShip >= 0) {
            // Clicked on a waypoint
            int si = contextMenuLegShip;
            int li = contextMenuHitLeg;
            char hdr[64];
            snprintf(hdr, sizeof(hdr), "Ship %d / Leg %d", si + 1, li + 1);
            ImGui::TextDisabled("%s", hdr);
            ImGui::Separator();

            if (ImGui::MenuItem("Select ship")) {
                shipPlacer.selectShip(si);
            }
            if (ImGui::MenuItem("Delete leg")) {
                if (si < (int)scenarioData.otherShipsData.size()) {
                    ScenarioData before = scenarioData;
                    auto& ship = scenarioData.otherShipsData[si];
                    if (li < (int)ship.legs.size()) {
                        ship.legs.erase(ship.legs.begin() + li);
                        scenarioDirty = true;
                        ScenarioData after = scenarioData;
                        undoStack.push("Delete leg",
                            [this, before]() { scenarioData = before; scenarioDirty = true; },
                            [this, after]() { scenarioData = after; scenarioDirty = true; });
                    }
                }
            }
            if (ImGui::MenuItem("Insert leg before")) {
                if (si < (int)scenarioData.otherShipsData.size()) {
                    ScenarioData before = scenarioData;
                    auto& ship = scenarioData.otherShipsData[si];
                    if (li < (int)ship.legs.size()) {
                        LegData newLeg;
                        newLeg.bearing = ship.legs[li].bearing;
                        newLeg.speed = ship.legs[li].speed;
                        newLeg.distance = 0.5f;
                        newLeg.startTime = 0;
                        ship.legs.insert(ship.legs.begin() + li, newLeg);
                        scenarioDirty = true;
                        ScenarioData after = scenarioData;
                        undoStack.push("Insert leg",
                            [this, before]() { scenarioData = before; scenarioDirty = true; },
                            [this, after]() { scenarioData = after; scenarioDirty = true; });
                    }
                }
            }
        } else if (contextMenuHitShip >= -1) {
            // Clicked on a ship
            if (contextMenuHitShip == -1) {
                ImGui::TextDisabled("Own Ship");
            } else {
                char hdr[64];
                snprintf(hdr, sizeof(hdr), "Ship %d", contextMenuHitShip + 1);
                ImGui::TextDisabled("%s", hdr);
            }
            ImGui::Separator();

            if (ImGui::MenuItem("Select")) {
                shipPlacer.selectShip(contextMenuHitShip);
                shipPlacer.currentTool = ShipPlacer::SELECT;
            }
            if (contextMenuHitShip >= 0) {
                if (ImGui::MenuItem("Add waypoint...")) {
                    shipPlacer.selectShip(contextMenuHitShip);
                    shipPlacer.currentTool = ShipPlacer::PLACE_WAYPOINT;
                }
                if (ImGui::MenuItem("Delete ship")) {
                    int si = contextMenuHitShip;
                    if (si < (int)scenarioData.otherShipsData.size()) {
                        ScenarioData before = scenarioData;
                        scenarioData.otherShipsData.erase(scenarioData.otherShipsData.begin() + si);
                        if (shipPlacer.getSelectedShip() == si) shipPlacer.selectShip(-2);
                        else if (shipPlacer.getSelectedShip() > si)
                            shipPlacer.selectShip(shipPlacer.getSelectedShip() - 1);
                        scenarioDirty = true;
                        ScenarioData after = scenarioData;
                        int selAfter = shipPlacer.getSelectedShip();
                        undoStack.push("Delete ship",
                            [this, before, si]() {
                                scenarioData = before;
                                shipPlacer.selectShip(si);
                                scenarioDirty = true;
                            },
                            [this, after, selAfter]() {
                                scenarioData = after;
                                shipPlacer.selectShip(selAfter);
                                scenarioDirty = true;
                            });
                    }
                }
            }
        } else {
            // Empty water
            ImGui::TextDisabled("%.4f, %.4f", contextMenuLat, contextMenuLon);
            ImGui::Separator();

            if (ImGui::MenuItem("Place own ship here")) {
                ScenarioData before = scenarioData;
                scenarioData.ownShipData.initialLat = (float)contextMenuLat;
                scenarioData.ownShipData.initialLong = (float)contextMenuLon;
                shipPlacer.selectShip(-1);
                shipPlacer.currentTool = ShipPlacer::SELECT;
                scenarioDirty = true;
                ScenarioData after = scenarioData;
                undoStack.push("Place own ship",
                    [this, before]() { scenarioData = before; scenarioDirty = true; },
                    [this, after]() { scenarioData = after; scenarioDirty = true; });
            }
            if (ImGui::MenuItem("Add other ship here")) {
                ScenarioData before = scenarioData;
                OtherShipData ship;
                ship.initialLat = (float)contextMenuLat;
                ship.initialLong = (float)contextMenuLon;
                ship.shipName = "Ship";
                ship.mmsi = 0;
                ship.drifting = false;
                LegData stopLeg;
                stopLeg.bearing = 0;
                stopLeg.speed = 0;
                stopLeg.distance = 0;
                stopLeg.startTime = 0;
                ship.legs.push_back(stopLeg);
                scenarioData.otherShipsData.push_back(std::move(ship));
                int newIdx = (int)scenarioData.otherShipsData.size() - 1;
                shipPlacer.selectShip(newIdx);
                shipPlacer.currentTool = ShipPlacer::SELECT;
                scenarioDirty = true;
                ScenarioData after = scenarioData;
                undoStack.push("Add ship",
                    [this, before]() { scenarioData = before; shipPlacer.selectShip(-2); scenarioDirty = true; },
                    [this, after, newIdx]() { scenarioData = after; shipPlacer.selectShip(newIdx); scenarioDirty = true; });
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Measure from here")) {
                measureTool.clear();
                measureTool.handleClick(contextMenuLat, contextMenuLon);
            }
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}

void EditorApp::renderPropertiesPanel(float x, float y, float w, float h) {
    ImGui::SetNextWindowPos(ImVec2(x, y));
    ImGui::SetNextWindowSize(ImVec2(w, h));

    ImGui::Begin("Properties", nullptr,
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse);

    if (ImGui::CollapsingHeader("Scenario", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Scenario name (editable buffer backed by scenarioData)
        static char nameBuf[128] = {};
        static bool nameInit = false;
        if (!nameInit) {
            strncpy_s(nameBuf, sizeof(nameBuf), scenarioData.scenarioName.c_str(), _TRUNCATE);
            nameInit = true;
        }
        if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf)))
            scenarioData.scenarioName = nameBuf;

        // Description
        static char descBuf[512] = {};
        static bool descInit = false;
        if (!descInit) {
            strncpy_s(descBuf, sizeof(descBuf), scenarioData.description.c_str(), _TRUNCATE);
            descInit = true;
        }
        if (ImGui::InputTextMultiline("Description", descBuf, sizeof(descBuf), ImVec2(-1, 60)))
            scenarioData.description = descBuf;

        // Start time (stored as seconds, edit as hours)
        float startHours = scenarioData.startTime / 3600.0f;
        if (ImGui::SliderFloat("Start Time", &startHours, 0.0f, 24.0f, "%.1f hr"))
            scenarioData.startTime = startHours * 3600.0f;

        // Date
        int day = scenarioData.startDay;
        int month = scenarioData.startMonth;
        int year = scenarioData.startYear;
        ImGui::PushItemWidth(60);
        if (ImGui::InputInt("Day", &day, 0)) scenarioData.startDay = (uint32_t)std::max(1, std::min(31, day));
        ImGui::SameLine();
        if (ImGui::InputInt("Month", &month, 0)) scenarioData.startMonth = (uint32_t)std::max(1, std::min(12, month));
        ImGui::SameLine();
        ImGui::PushItemWidth(80);
        if (ImGui::InputInt("Year", &year, 0)) scenarioData.startYear = (uint32_t)std::max(2000, year);
        ImGui::PopItemWidth();
        ImGui::PopItemWidth();
    }

    if (ImGui::CollapsingHeader("Environment", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Weather", &scenarioData.weather, 0.0f, 12.0f, "%.1f");
        ImGui::SliderFloat("Visibility (nm)", &scenarioData.visibilityRange, 0.1f, 30.0f, "%.1f");
        ImGui::SliderFloat("Wind Dir", &scenarioData.windDirection, 0.0f, 360.0f, "%.0f deg");
        ImGui::SliderFloat("Wind Speed (kts)", &scenarioData.windSpeed, 0.0f, 60.0f, "%.1f");
        ImGui::SliderFloat("Rain", &scenarioData.rainIntensity, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Sunrise", &scenarioData.sunRise, 0.0f, 12.0f, "%.1f hr");
        ImGui::SliderFloat("Sunset", &scenarioData.sunSet, 12.0f, 24.0f, "%.1f hr");
    }

    if (ImGui::CollapsingHeader("Own Ship", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Ship model dropdown
        if (ImGui::BeginCombo("Model##own", scenarioData.ownShipData.ownShipName.c_str())) {
            for (const auto& model : ownShipModels) {
                bool selected = (model == scenarioData.ownShipData.ownShipName);
                if (ImGui::Selectable(model.c_str(), selected)) {
                    scenarioData.ownShipData.ownShipName = model;
                    scenarioDirty = true;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        ImGui::Text("Lat: %.4f  Lon: %.4f",
                    scenarioData.ownShipData.initialLat,
                    scenarioData.ownShipData.initialLong);
        ImGui::SliderFloat("Bearing##own", &scenarioData.ownShipData.initialBearing, 0.0f, 360.0f, "%.0f deg");
        ImGui::InputFloat("Speed (kts)##own", &scenarioData.ownShipData.initialSpeed, 1.0f, 5.0f, "%.1f");

        if (ImGui::Button("Place on Map##own")) {
            shipPlacer.currentTool = ShipPlacer::PLACE_OWNSHIP;
        }
    }

    if (ImGui::CollapsingHeader("Other Ships", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (scenarioData.otherShipsData.empty()) {
            ImGui::Text("No other ships placed.");
        }

        int shipToDelete = -1;
        for (size_t i = 0; i < scenarioData.otherShipsData.size(); i++) {
            auto& ship = scenarioData.otherShipsData[i];
            ImGui::PushID((int)i);

            bool isSelected = (shipPlacer.getSelectedShip() == (int)i);
            char header[64];
            snprintf(header, sizeof(header), "Ship %zu: %s###ship%zu",
                     i + 1, ship.shipName.c_str(), i);

            if (ImGui::TreeNode(header)) {
                if (!isSelected && ImGui::IsItemClicked())
                    shipPlacer.selectShip((int)i);

                // Ship model dropdown
                if (ImGui::BeginCombo("Model", ship.shipName.c_str())) {
                    for (const auto& model : otherShipModels) {
                        bool selected = (model == ship.shipName);
                        if (ImGui::Selectable(model.c_str(), selected)) {
                            ship.shipName = model;
                            scenarioDirty = true;
                        }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }

                ImGui::Text("Lat: %.4f  Lon: %.4f", ship.initialLat, ship.initialLong);

                int mmsi = (int)ship.mmsi;
                if (ImGui::InputInt("MMSI", &mmsi))
                    ship.mmsi = (uint32_t)std::max(0, mmsi);

                ImGui::Checkbox("Drifting", &ship.drifting);

                // Legs
                if (ImGui::TreeNode("Legs")) {
                    for (size_t j = 0; j < ship.legs.size(); j++) {
                        auto& leg = ship.legs[j];
                        if (leg.speed == 0 && leg.distance == 0 && j == ship.legs.size() - 1) {
                            ImGui::TextDisabled("(stop)");
                            continue;
                        }
                        ImGui::PushID((int)j);
                        ImGui::PushItemWidth(60);
                        ImGui::InputFloat("Brg", &leg.bearing, 0, 0, "%.0f");
                        ImGui::SameLine();
                        ImGui::InputFloat("Spd", &leg.speed, 0, 0, "%.1f");
                        ImGui::SameLine();
                        ImGui::InputFloat("Dist", &leg.distance, 0, 0, "%.2f");
                        ImGui::PopItemWidth();
                        ImGui::PopID();
                    }
                    ImGui::TreePop();
                }

                if (ImGui::Button("Delete Ship"))
                    shipToDelete = (int)i;

                ImGui::TreePop();
            }
            ImGui::PopID();
        }

        if (shipToDelete >= 0 && shipToDelete < (int)scenarioData.otherShipsData.size()) {
            scenarioData.otherShipsData.erase(scenarioData.otherShipsData.begin() + shipToDelete);
            if (shipPlacer.getSelectedShip() >= (int)scenarioData.otherShipsData.size())
                shipPlacer.selectShip(-2);
        }

        if (ImGui::Button("Add Ship on Map")) {
            shipPlacer.currentTool = ShipPlacer::PLACE_OTHERSHIP;
        }
    }

    ImGui::End();
}

void EditorApp::renderStatusBar(float topY) {
    ImGui::SetNextWindowPos(ImVec2(0, topY));
    ImGui::SetNextWindowSize(ImVec2((float)windowWidth, STATUS_BAR_HEIGHT));

    ImGui::Begin("##StatusBar", nullptr,
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar);

    ImGui::Text("Mouse: %.4f, %.4f  |  Center: %.4f, %.4f  |  Zoom: %d  |  %s%s",
                mouseLat, mouseLon, mapCenterLat, mapCenterLon, mapZoom,
                tileSource == TILE_SATELLITE ? "Satellite" : "Street",
                seamarkOverlay ? " +Seamark" : "");
    if (areaSelector.getState() == AreaSelector::COMPLETE) {
        ImGui::SameLine();
        ImGui::Text("  |  Area: %.1f x %.1f km",
                    areaSelector.getWidthKm(), areaSelector.getHeightKm());
    }
    if (!statusMessage.empty()) {
        ImGui::SameLine();
        ImGui::Text("  |  %s", statusMessage.c_str());
    }

    ImGui::End();
}

void EditorApp::renderGenerateWorldDialog() {
    ImGui::SetNextWindowSize(ImVec2(450, 0), ImGuiCond_Always);
    ImGui::SetNextWindowPos(
        ImVec2((float)windowWidth * 0.5f, (float)windowHeight * 0.5f),
        ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (!ImGui::Begin("Generate World", &showGenerateDialog,
                       ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize)) {
        ImGui::End();
        return;
    }

    // Area info
    ImGui::Text("Area: %.4f to %.4f N, %.4f to %.4f E",
                areaSelector.getMinLat(), areaSelector.getMaxLat(),
                areaSelector.getMinLon(), areaSelector.getMaxLon());
    ImGui::Text("Size: %.1f km x %.1f km",
                areaSelector.getWidthKm(), areaSelector.getHeightKm());
    ImGui::Separator();

    // World name
    ImGui::InputText("World name", worldNameBuf, sizeof(worldNameBuf));

    // Resolution dropdown
    const char* resOptions[] = { "257", "513", "1025", "2049", "4097" };
    int resValues[] = { 257, 513, 1025, 2049, 4097 };
    int currentRes = 2; // default 1025
    for (int i = 0; i < 5; i++) {
        if (resValues[i] == worldResolution) currentRes = i;
    }
    if (ImGui::Combo("Resolution", &currentRes, resOptions, 5)) {
        worldResolution = resValues[currentRes];
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(pixels per side)");

    ImGui::Separator();

    // Chart status
    if (chartOverlay.isLoaded()) {
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "S-57 Chart loaded (%d chart(s))",
                           chartOverlay.chartCount());
        ImGui::Text("  Buoys, lights, coastlines, and depth will be extracted");
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "No S-57 chart loaded");
        ImGui::Text("  Load a .000 file (toolbar 'C' key) for buoys/coastline/depth");
    }

    ImGui::Separator();
    ImGui::Text("Data sources:");
    ImGui::Checkbox("GEBCO bathymetry (ocean depth)", &useGEBCO);
    ImGui::Checkbox("Copernicus DEM (land elevation)", &useCopernicusDEM);
    ImGui::Checkbox("Use satellite imagery for texture", &useSatelliteTexture);
    if (useSatelliteTexture) {
        ImGui::SameLine();
        ImGui::TextDisabled("(downloads ESRI tiles)");
    }

    if (!chartOverlay.isLoaded()) {
        ImGui::Checkbox("Use OpenSeaMap data (buoys, lights, landmarks)", &useOpenSeaMap);
        if (useOpenSeaMap) {
            ImGui::SameLine();
            ImGui::TextDisabled("(free worldwide)");
        }
    }

    ImGui::Separator();

    bool canGenerate = !isGenerating && strlen(worldNameBuf) > 0;
    if (!canGenerate) ImGui::BeginDisabled();
    if (ImGui::Button("Generate World", ImVec2(200, 0))) {
        generateWorldFromArea();
    }
    if (!canGenerate) ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100, 0))) {
        showGenerateDialog = false;
    }

    if (isGenerating) {
        ImGui::Separator();
        float frac = generateTilesTotal > 0
            ? static_cast<float>(generateTilesReady) / generateTilesTotal : 0.0f;
        char overlay[64];
        snprintf(overlay, sizeof(overlay), "%d / %d tiles", generateTilesReady, generateTilesTotal);
        ImGui::ProgressBar(frac, ImVec2(-1, 0), overlay);
    }

    if (!generateStatus.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("Status: %s", generateStatus.c_str());
    }

    ImGui::End();
}

// --- File operations ---

void EditorApp::newScenario() {
    scenarioData = ScenarioData();
    scenarioData.sunRise = 6.0f;
    scenarioData.sunSet = 18.0f;
    scenarioData.visibilityRange = 10.0f;
    scenarioData.startDay = 1;
    scenarioData.startMonth = 1;
    scenarioData.startYear = 2026;
    // Set default own ship to first available model
    if (!ownShipModels.empty())
        scenarioData.ownShipData.ownShipName = ownShipModels.front();
    scenarioPath.clear();
    scenarioDirty = false;
    shipPlacer.currentTool = ShipPlacer::SELECT;
    shipPlacer.selectShip(-2);
    undoStack.clear();
    statusMessage = "New scenario created";
}

void EditorApp::openScenario(const std::string& name) {
    std::string fullPath = scenarioBaseDir + name;
    scenarioData = ScenarioFileIO::load(fullPath);
    scenarioData.scenarioName = name;
    scenarioPath = fullPath;
    scenarioDirty = false;
    shipPlacer.currentTool = ShipPlacer::SELECT;
    shipPlacer.selectShip(-2);

    // Load world buoys/lights
    loadWorldData(scenarioData.worldName);

    // Center map on own ship if placed
    if (scenarioData.ownShipData.initialLat != 0 || scenarioData.ownShipData.initialLong != 0) {
        mapCenterLat = scenarioData.ownShipData.initialLat;
        mapCenterLon = scenarioData.ownShipData.initialLong;
        mapZoom = 12;
    }

    statusMessage = "Opened scenario: " + name;
}

bool EditorApp::saveScenario() {
    if (scenarioPath.empty() || scenarioData.scenarioName.empty()) {
        showSaveAsDialog = true;
        return false;
    }
    bool ok = ScenarioFileIO::save(scenarioPath, scenarioData);
    if (ok) {
        scenarioDirty = false;
        statusMessage = "Saved: " + scenarioData.scenarioName;
    } else {
        statusMessage = "Failed to save scenario!";
    }
    return ok;
}

bool EditorApp::saveScenarioAs(const std::string& name) {
    scenarioData.scenarioName = name;
    scenarioPath = scenarioBaseDir + name;
    return saveScenario();
}

void EditorApp::refreshScenarioList() {
    availableScenarios = ScenarioFileIO::listScenarios(scenarioBaseDir);

    // Also check user dir
    std::string userScenarios = Utilities::getUserDir() + "Scenarios/";
    if (Utilities::pathExists(userScenarios) && userScenarios != scenarioBaseDir) {
        auto userList = ScenarioFileIO::listScenarios(userScenarios);
        for (auto& s : userList) {
            availableScenarios.push_back(s);
        }
    }
}

void EditorApp::testInSimulator() {
    // Auto-save first
    if (scenarioPath.empty() || scenarioData.scenarioName.empty()) {
        statusMessage = "Save the scenario first before testing";
        showSaveAsDialog = true;
        return;
    }
    saveScenario();

#ifdef _WIN32
    // Launch simulator from the same directory as the editor
    ShellExecuteA(nullptr, nullptr, "bridgecommand-bc.exe", nullptr, nullptr, SW_SHOW);
    statusMessage = "Launched simulator - select scenario: " + scenarioData.scenarioName;
#else
    statusMessage = "Test in Simulator not yet implemented on this platform";
#endif
}

void EditorApp::exportScenarioZip(bool includeWorld) {
    // Must save first
    if (scenarioPath.empty() || scenarioData.scenarioName.empty()) {
        statusMessage = "Save the scenario first before exporting";
        showSaveAsDialog = true;
        return;
    }
    saveScenario();

#ifdef _WIN32
    char filePath[MAX_PATH] = {};
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = "ZIP Files (*.zip)\0*.zip\0";
    ofn.lpstrFile = filePath;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = includeWorld ? "Export Scenario with World" : "Export Scenario";
    ofn.lpstrDefExt = "zip";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;

    // Default filename based on scenario name
    strncpy_s(filePath, sizeof(filePath), (scenarioData.scenarioName + ".zip").c_str(), _TRUNCATE);

    if (!GetSaveFileNameA(&ofn))
        return;

    mz_zip_archive zip = {};
    if (!mz_zip_writer_init_file(&zip, filePath, 0)) {
        statusMessage = "Failed to create ZIP file";
        return;
    }

    namespace fs = std::filesystem;

    // Add scenario files
    std::string scenPrefix = "Scenarios/" + scenarioData.scenarioName + "/";
    for (auto& entry : fs::directory_iterator(scenarioPath)) {
        if (entry.is_regular_file()) {
            std::string fname = entry.path().filename().string();
            std::string archiveName = scenPrefix + fname;
            std::string diskPath = entry.path().string();
            mz_zip_writer_add_file(&zip, archiveName.c_str(), diskPath.c_str(),
                                   nullptr, 0, MZ_DEFAULT_COMPRESSION);
        }
    }

    // Optionally include world directory
    if (includeWorld && !scenarioData.worldName.empty()) {
        std::string worldDir = "World/" + scenarioData.worldName;
        std::string userFolder = Utilities::getUserDir();
        std::string worldDiskPath = worldDir;
        if (Utilities::pathExists(userFolder + worldDir)) {
            worldDiskPath = userFolder + worldDir;
        }

        if (fs::exists(worldDiskPath)) {
            std::string worldPrefix = "World/" + scenarioData.worldName + "/";
            for (auto& entry : fs::recursive_directory_iterator(worldDiskPath)) {
                if (entry.is_regular_file()) {
                    std::string relPath = fs::relative(entry.path(), worldDiskPath).string();
                    // Normalize to forward slashes
                    std::replace(relPath.begin(), relPath.end(), '\\', '/');
                    std::string archiveName = worldPrefix + relPath;
                    std::string diskPath = entry.path().string();
                    mz_zip_writer_add_file(&zip, archiveName.c_str(), diskPath.c_str(),
                                           nullptr, 0, MZ_DEFAULT_COMPRESSION);
                }
            }
        }
    }

    mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);
    statusMessage = "Exported: " + std::string(filePath);
#endif
}

void EditorApp::importScenarioZip() {
#ifdef _WIN32
    char filePath[MAX_PATH] = {};
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = "ZIP Files (*.zip)\0*.zip\0";
    ofn.lpstrFile = filePath;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = "Import Scenario from ZIP";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetOpenFileNameA(&ofn))
        return;

    // Determine base extraction directory (user dir or current dir)
    std::string baseDir = Utilities::getUserDir();
    if (baseDir.empty()) baseDir = "./";

    mz_zip_archive zip = {};
    if (!mz_zip_reader_init_file(&zip, filePath, 0)) {
        statusMessage = "Failed to open ZIP file";
        return;
    }

    namespace fs = std::filesystem;
    int numFiles = (int)mz_zip_reader_get_num_files(&zip);
    std::string importedScenarioName;

    for (int i = 0; i < numFiles; i++) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat))
            continue;
        if (stat.m_is_directory)
            continue;

        std::string archiveName = stat.m_filename;

        // Determine the scenario name from first Scenarios/ entry
        if (importedScenarioName.empty() && archiveName.find("Scenarios/") == 0) {
            size_t slashPos = archiveName.find('/', 10); // After "Scenarios/"
            if (slashPos != std::string::npos) {
                importedScenarioName = archiveName.substr(10, slashPos - 10);
            }
        }

        // Extract to baseDir + archiveName
        std::string destPath = baseDir + archiveName;

        // Create parent directories
        fs::path destParent = fs::path(destPath).parent_path();
        fs::create_directories(destParent);

        mz_zip_reader_extract_to_file(&zip, i, destPath.c_str(), 0);
    }

    mz_zip_reader_end(&zip);

    if (!importedScenarioName.empty()) {
        statusMessage = "Imported scenario: " + importedScenarioName;
        // Refresh and open
        refreshScenarioList();
        openScenario(importedScenarioName);
    } else {
        statusMessage = "Imported ZIP (no scenario found)";
    }
#endif
}

void EditorApp::generateWorldFromArea() {
    if (isGenerating) return;

    std::string worldName(worldNameBuf);
    if (worldName.empty()) {
        generateStatus = "Please enter a world name";
        return;
    }

    double minLat = areaSelector.getMinLat();
    double maxLat = areaSelector.getMaxLat();
    double minLon = areaSelector.getMinLon();
    double maxLon = areaSelector.getMaxLon();

    // Create output directory
    std::string userDir = Utilities::getUserDir();
    std::string outputDir = userDir + "World/" + worldName;

    namespace fs = std::filesystem;
    try {
        fs::create_directories(outputDir);
    } catch (const std::exception& e) {
        generateStatus = std::string("Failed to create directory: ") + e.what();
        return;
    }

    int resolution = worldResolution;
    bool wantSatellite = useSatelliteTexture;
    bool wantOpenSeaMap = useOpenSeaMap;
    bool hasChart = chartOverlay.isLoaded();

#ifdef WITH_GDAL
    // ---- GDAL path: use S-57 chart data for full world generation ----
    if (hasChart && !chartOverlay.getChartPaths().empty()) {
        isGenerating = true;
        generateStatus = "Generating world from chart data...";
        generateTilesReady = 0;
        generateTilesTotal = 1;

        std::string chartPath = chartOverlay.getChartPaths()[0]; // Primary chart
        std::string cacheDir = userDir + "tilecache";

        std::thread([this, chartPath, outputDir, resolution, wantSatellite,
                     cacheDir, minLat, maxLat, minLon, maxLon]() {
            // Generate world from chart data (heightmap, buoys, lights, terrain)
            auto result = WorldGenerator::generateWorld(chartPath, outputDir, resolution);

            if (result.success) {
                generateStatus = "Chart world: " + std::to_string(result.buoyCount) + " buoys, "
                    + std::to_string(result.lightCount) + " lights, "
                    + std::to_string(result.landmarkCount) + " landmarks";

                // Optionally overlay satellite imagery on top of the generated texture
                if (wantSatellite) {
                    int outW = 0, outH = 0;
                    auto progressCb = [this](int ready, int total) -> bool {
                        generateTilesReady = ready;
                        generateTilesTotal = total;
                        return true;
                    };
                    int zoom = SatelliteTexture::suggestZoom(minLat, maxLat, minLon, maxLon, resolution);
                    auto texData = SatelliteTexture::generate(
                        minLat, maxLat, minLon, maxLon,
                        zoom, resolution, cacheDir, outW, outH, progressCb);
                    if (!texData.empty()) {
                        SatelliteTexture::writePNG(outputDir + "/texture.png", texData, outW, outH);
                        generateStatus += " + satellite texture";
                    }
                }
            } else {
                generateStatus = "World generation failed - check chart data";
            }

            isGenerating = false;
        }).detach();

        scenarioData.worldName = worldName;
        scenarioDirty = true;
        return;
    }
#endif // WITH_GDAL

    // ---- Fallback path: no chart data, generate basic world ----
    double lonExtent = maxLon - minLon;
    double latExtent = maxLat - minLat;

    // Write terrain.ini
    {
        std::ofstream f(outputDir + "/terrain.ini");
        if (!f.is_open()) {
            generateStatus = "Failed to write terrain.ini";
            return;
        }
        f << std::fixed;
        f.precision(14);
        f << "Number=1\n";
        f << "MapImage=map.png\n";
        f << "HeightMap(1)=height.png\n";
        f << "Texture(1)=texture.png\n";
        f << "TerrainLong(1)=" << minLon << "\n";
        f << "TerrainLat(1)=" << minLat << "\n";
        f << "TerrainLongExtent(1)=" << lonExtent << "\n";
        f << "TerrainLatExtent(1)=" << latExtent << "\n";
        f << "TerrainHeightMapRows(1)=" << resolution << "\n";
        f << "TerrainHeightMapColumns(1)=" << resolution << "\n";
        f.precision(5);
        f << "TerrainMaxHeight(1)=5.00000\n";
        f << "SeaMaxDepth(1)=50.00000\n";
        f << "UsesRGB(1)=1\n";
    }

    // Write tide/tidalstream (always empty for now)
    {
        std::ofstream f1(outputDir + "/tide.ini");
        if (f1.is_open()) f1 << "Number=0\n";
        std::ofstream f2(outputDir + "/tidalstream.ini");
        if (f2.is_open()) f2 << "Number=0\n";
    }

    // Background thread: satellite download + OpenSeaMap query + coastline heightmap
    isGenerating = true;
    generateTilesReady = 0;
    generateTilesTotal = 0;
    generateStatus = wantOpenSeaMap ? "Querying OpenSeaMap..." : "Generating world...";
    std::string cacheDir = userDir + "tilecache";

    // Resolve coastline data path (absolute) before thread starts
    std::string coastlinePath;
    {
        namespace fs = std::filesystem;
        const char* candidates[] = {
            "Data/Coastlines/coastlines_10m.bin",
            "bin/Data/Coastlines/coastlines_10m.bin",
            "../bin/Data/Coastlines/coastlines_10m.bin",
            "Data/Coastlines/coastlines_50m.bin",
            "bin/Data/Coastlines/coastlines_50m.bin",
            "../bin/Data/Coastlines/coastlines_50m.bin",
        };
        for (auto& p : candidates) {
            if (fs::exists(p)) {
                coastlinePath = fs::absolute(p).string();
                break;
            }
        }
    }

    std::thread([this, minLat, maxLat, minLon, maxLon, resolution,
                 cacheDir, outputDir, wantSatellite, wantOpenSeaMap,
                 coastlinePath]() {
        std::string resultMsg;
        int buoyCount = 0, lightCount = 0, landmarkCount = 0;

        // Load coastlines ONCE (used for both heightmap and building filtering)
        CoastlineData coastlines;
        bool hasCoastlines = !coastlinePath.empty() && coastlines.load(coastlinePath);
        if (hasCoastlines) {
            coastlines.prefilter(minLon, maxLon, minLat, maxLat);
        }

        // Query OpenSeaMap for buoys, lights, landmarks
        if (wantOpenSeaMap) {
            generateStatus = "Querying OpenSeaMap for seamark data...";
            OpenSeaMapSource osm;
            auto progress = [this](const std::string& msg) {
                generateStatus = msg;
            };
            if (osm.query(minLat, maxLat, minLon, maxLon, progress)) {
                buoyCount = (int)osm.getBuoys().size();
                lightCount = (int)osm.getLights().size();
                landmarkCount = (int)osm.getLandmarks().size();

                // Write populated INI files
                {
                    std::ofstream f(outputDir + "/buoy.ini");
                    if (f.is_open()) f << osm.generateBuoyIni();
                }
                {
                    std::ofstream f(outputDir + "/light.ini");
                    if (f.is_open()) f << osm.generateLightIni();
                }
                {
                    std::ofstream f(outputDir + "/landobject.ini");
                    if (f.is_open()) f << osm.generateLandObjectIni();
                }

                resultMsg = std::to_string(buoyCount) + " buoys, "
                    + std::to_string(lightCount) + " lights, "
                    + std::to_string(landmarkCount) + " landmarks";
            } else {
                // Query failed - write empty INIs
                std::ofstream f1(outputDir + "/buoy.ini");
                if (f1.is_open()) f1 << "Number=0\n";
                std::ofstream f2(outputDir + "/light.ini");
                if (f2.is_open()) f2 << "Number=0\n";
                std::ofstream f3(outputDir + "/landobject.ini");
                if (f3.is_open()) f3 << "Number=0\n";
                resultMsg = "OpenSeaMap query failed: " + osm.getError();
            }
        } else {
            // No OpenSeaMap - write empty INIs
            std::ofstream f1(outputDir + "/buoy.ini");
            if (f1.is_open()) f1 << "Number=0\n";
            std::ofstream f2(outputDir + "/light.ini");
            if (f2.is_open()) f2 << "Number=0\n";
            std::ofstream f3(outputDir + "/landobject.ini");
            if (f3.is_open()) f3 << "Number=0\n";
        }

        // Generate heightmap: Natural Earth base + OSM barrier enclosed-area detection.
        // Strategy:
        //   1. Natural Earth per-pixel classification (reliable land/sea for most areas)
        //   2. Query OSM for barriers (dams, breakwaters) - NOT coastlines
        //   3. Rasterize barriers as boundary lines
        //   4. Flood "open sea" from grid edges through NE-water pixels, blocked by barriers
        //   5. NE-water pixels NOT reached by open-sea flood = enclosed by barriers = land
        // This correctly handles Cardiff Bay (barrage blocks flood) while keeping
        // Natural Earth's reliable land classification everywhere else.
        generateStatus = "Generating heightmap...";
        std::vector<float> heightGrid(resolution * resolution, -20.0f);
        std::vector<std::vector<std::pair<double,double>>> osmBarriers;
        {
            // Step 1: Natural Earth per-pixel classification
            for (int py = 0; py < resolution; py++) {
                double lat = maxLat - (maxLat - minLat) * py / (resolution - 1);
                for (int px = 0; px < resolution; px++) {
                    double lon = minLon + (maxLon - minLon) * px / (resolution - 1);
                    if (hasCoastlines && coastlines.isLand(lon, lat)) {
                        heightGrid[py * resolution + px] = 2.0f;
                    }
                }
            }

            // Step 2: Query OSM for barriers only (dams, breakwaters)
            generateStatus = "Querying OSM for barriers (dams, breakwaters)...";
            {
                using njson = nlohmann::json;
                std::ostringstream ql;
                ql << std::fixed;
                ql.precision(6);
                ql << "[out:json][timeout:60];"
                   << "(way[\"waterway\"=\"dam\"]("
                   << minLat << "," << minLon << "," << maxLat << "," << maxLon
                   << ");"
                   << "way[\"man_made\"=\"breakwater\"]("
                   << minLat << "," << minLon << "," << maxLat << "," << maxLon
                   << "););"
                   << "out geom;";

                std::string postBody = "data=" + ql.str();
                auto response = OSMBuildingReader::httpPost(
                    "https://overpass-api.de/api/interpreter", postBody,
                    "BridgeCommand/6.0 (world-generator)");

                if (!response.empty()) {
                    try {
                        auto root = njson::parse(response.begin(), response.end());
                        if (root.contains("elements") && root["elements"].is_array()) {
                            for (const auto& elem : root["elements"]) {
                                if (!elem.contains("geometry") || !elem["geometry"].is_array())
                                    continue;
                                std::vector<std::pair<double,double>> polyline;
                                for (const auto& pt : elem["geometry"]) {
                                    if (pt.contains("lat") && pt.contains("lon")) {
                                        polyline.emplace_back(
                                            pt["lat"].get<double>(),
                                            pt["lon"].get<double>());
                                    }
                                }
                                if (polyline.size() >= 2)
                                    osmBarriers.push_back(std::move(polyline));
                            }
                        }
                    } catch (...) {}
                }
            }

            // Step 3-5: If barriers found, reclaim enclosed water areas as land
            if (!osmBarriers.empty()) {
                generateStatus = "Detecting barrier-enclosed areas...";

                auto lonToPixel = [&](double lon) -> int {
                    return std::max(0, std::min(resolution - 1,
                        (int)((lon - minLon) / (maxLon - minLon) * (resolution - 1))));
                };
                auto latToPixel = [&](double lat) -> int {
                    return std::max(0, std::min(resolution - 1,
                        (int)((maxLat - lat) / (maxLat - minLat) * (resolution - 1))));
                };

                // Rasterize barrier ways as boundary pixels (Bresenham)
                std::vector<bool> isBoundary(resolution * resolution, false);
                auto rasterizeLine = [&](int x0, int y0, int x1, int y1) {
                    int ddx = std::abs(x1 - x0), ddy = std::abs(y1 - y0);
                    int sx = (x0 < x1) ? 1 : -1;
                    int sy = (y0 < y1) ? 1 : -1;
                    int err = ddx - ddy;
                    while (true) {
                        if (x0 >= 0 && x0 < resolution && y0 >= 0 && y0 < resolution)
                            isBoundary[y0 * resolution + x0] = true;
                        if (x0 == x1 && y0 == y1) break;
                        int e2 = 2 * err;
                        if (e2 > -ddy) { err -= ddy; x0 += sx; }
                        if (e2 < ddx) { err += ddx; y0 += sy; }
                    }
                };

                for (const auto& barrier : osmBarriers) {
                    for (size_t i = 1; i < barrier.size(); i++) {
                        rasterizeLine(
                            lonToPixel(barrier[i-1].second), latToPixel(barrier[i-1].first),
                            lonToPixel(barrier[i].second),   latToPixel(barrier[i].first));
                    }
                }

                // Dilate boundary by 1 pixel to close diagonal gaps
                {
                    const int d4x[] = {-1, 1, 0, 0};
                    const int d4y[] = {0, 0, -1, 1};
                    std::vector<bool> dilated = isBoundary;
                    for (int py = 0; py < resolution; py++) {
                        for (int px = 0; px < resolution; px++) {
                            if (isBoundary[py * resolution + px]) {
                                for (int d = 0; d < 4; d++) {
                                    int nx = px + d4x[d], ny = py + d4y[d];
                                    if (nx >= 0 && nx < resolution && ny >= 0 && ny < resolution)
                                        dilated[ny * resolution + nx] = true;
                                }
                            }
                        }
                    }
                    isBoundary = std::move(dilated);
                }

                // Flood "open sea" from grid edges through water pixels, blocked by barriers.
                // Water pixels NOT reached = enclosed by barriers = should be land.
                std::vector<bool> openSea(resolution * resolution, false);
                std::queue<int> fillQueue;

                // Seed: edge pixels that are water and not on a barrier
                for (int px = 0; px < resolution; px++) {
                    for (int py : {0, resolution - 1}) {
                        int idx = py * resolution + px;
                        if (heightGrid[idx] < 0.0f && !isBoundary[idx]) {
                            openSea[idx] = true;
                            fillQueue.push(idx);
                        }
                    }
                }
                for (int py = 1; py < resolution - 1; py++) {
                    for (int px : {0, resolution - 1}) {
                        int idx = py * resolution + px;
                        if (heightGrid[idx] < 0.0f && !isBoundary[idx]) {
                            openSea[idx] = true;
                            fillQueue.push(idx);
                        }
                    }
                }

                // BFS: flood through water pixels, blocked by barriers and land
                const int d4x[] = {-1, 1, 0, 0};
                const int d4y[] = {0, 0, -1, 1};
                while (!fillQueue.empty()) {
                    int idx = fillQueue.front();
                    fillQueue.pop();
                    int px = idx % resolution, py = idx / resolution;
                    for (int d = 0; d < 4; d++) {
                        int nx = px + d4x[d], ny = py + d4y[d];
                        if (nx < 0 || nx >= resolution || ny < 0 || ny >= resolution) continue;
                        int nidx = ny * resolution + nx;
                        if (!openSea[nidx] && !isBoundary[nidx] && heightGrid[nidx] < 0.0f) {
                            openSea[nidx] = true;
                            fillQueue.push(nidx);
                        }
                    }
                }

                // Convert enclosed water (not reached by open-sea flood) to land
                int reclaimedCount = 0;
                for (int i = 0; i < resolution * resolution; i++) {
                    if (heightGrid[i] < 0.0f && !openSea[i]) {
                        heightGrid[i] = 1.0f; // slightly lower than NE land (2.0f)
                        reclaimedCount++;
                    }
                    // Also mark barrier pixels as land
                    if (isBoundary[i] && heightGrid[i] < 0.0f) {
                        heightGrid[i] = 2.0f;
                    }
                }
            }

            // Smooth the coastline transition with multi-pass box blur.
            // Uses a 5x5 kernel for 8 passes to create a natural gradual slope
            // from sea to shore, eliminating sawtooth/staircase artifacts.
            std::vector<float> temp(resolution * resolution);
            for (int pass = 0; pass < 8; pass++) {
                for (int py = 0; py < resolution; py++) {
                    for (int px = 0; px < resolution; px++) {
                        float sum = 0;
                        int count = 0;
                        for (int dy = -2; dy <= 2; dy++) {
                            for (int dx = -2; dx <= 2; dx++) {
                                int ny = py + dy, nx = px + dx;
                                if (ny >= 0 && ny < resolution && nx >= 0 && nx < resolution) {
                                    sum += heightGrid[ny * resolution + nx];
                                    count++;
                                }
                            }
                        }
                        temp[py * resolution + px] = sum / count;
                    }
                }
                heightGrid = temp;
            }

            // Encode to RGB
            std::vector<uint8_t> heightRGB(resolution * resolution * 3);
            for (int py = 0; py < resolution; py++) {
                for (int px = 0; px < resolution; px++) {
                    float elevation = heightGrid[py * resolution + px];
                    float encoded = elevation + 32768.0f;
                    int intPart = static_cast<int>(encoded);
                    intPart = std::max(0, std::min(65535, intPart));

                    int idx = (py * resolution + px) * 3;
                    heightRGB[idx + 0] = static_cast<uint8_t>(intPart / 256);
                    heightRGB[idx + 1] = static_cast<uint8_t>(intPart % 256);
                    heightRGB[idx + 2] = 0;
                }
            }
            SatelliteTexture::writePNG(outputDir + "/height.png", heightRGB, resolution, resolution);
        }

        // Post-process buoy.ini: raise buoys near barrier structures so they sit
        // on top of the wall rather than at sea level. Uses point-to-segment distance
        // in geographic coords (converted to metres via Haversine approx).
        if (!osmBarriers.empty() && wantOpenSeaMap) {
            double midLat = (minLat + maxLat) * 0.5;
            double cosLat = std::cos(midLat * 3.14159265358979323846 / 180.0);
            double mPerDegLon = 111320.0 * cosLat;
            double mPerDegLat = 110540.0;
            const double BARRIER_PROXIMITY_M = 30.0; // metres
            const float BARRIER_WALL_TOP = 3.5f; // metres above sea level

            // Read existing buoy.ini to get lat/lon for each buoy
            std::string buoyIniPath = outputDir + "/buoy.ini";
            uint32_t numBuoys = IniFile::iniFileTou32(buoyIniPath, "Number");
            int raisedCount = 0;

            // Collect which buoys need raising
            std::vector<int> buoysToRaise;
            for (uint32_t b = 1; b <= numBuoys; b++) {
                double bLat = IniFile::iniFileTof32(buoyIniPath, IniFile::enumerate1("Lat", b));
                double bLon = IniFile::iniFileTof32(buoyIniPath, IniFile::enumerate1("Long", b));

                // Check distance to each barrier segment
                bool nearBarrier = false;
                for (const auto& barrier : osmBarriers) {
                    for (size_t i = 1; i < barrier.size() && !nearBarrier; i++) {
                        // Point-to-segment distance in metres
                        double ax = (barrier[i-1].second - bLon) * mPerDegLon;
                        double ay = (barrier[i-1].first - bLat) * mPerDegLat;
                        double bx = (barrier[i].second - bLon) * mPerDegLon;
                        double by = (barrier[i].first - bLat) * mPerDegLat;
                        double dx = bx - ax, dy = by - ay;
                        double lenSq = dx*dx + dy*dy;
                        double t = (lenSq > 0) ? std::max(0.0, std::min(1.0, (-ax*dx - ay*dy) / lenSq)) : 0.0;
                        double cx = ax + t*dx, cy = ay + t*dy;
                        double dist = std::sqrt(cx*cx + cy*cy);
                        if (dist < BARRIER_PROXIMITY_M)
                            nearBarrier = true;
                    }
                }
                if (nearBarrier)
                    buoysToRaise.push_back(b);
            }

            // Append HeightCorrection lines for affected buoys
            if (!buoysToRaise.empty()) {
                std::ofstream f(buoyIniPath, std::ios::app);
                if (f.is_open()) {
                    f << "\n# Barrier-proximity height corrections\n";
                    for (int b : buoysToRaise) {
                        f << "HeightCorrection(" << b << ")=" << BARRIER_WALL_TOP << "\n";
                        raisedCount++;
                    }
                }
            }

            if (raisedCount > 0) {
                resultMsg += ", " + std::to_string(raisedCount) + " buoys raised to barrier height";
            }
        }

        // Query OSM for building footprints and generate building mesh
        {
            generateStatus = "Querying OSM for buildings...";
            OSMBuildingReader bldgReader;
            if (bldgReader.query(minLat, maxLat, minLon, maxLon,
                                  [this](const std::string& msg) { generateStatus = msg; })) {
                const auto& footprints = bldgReader.getBuildings();
                if (!footprints.empty()) {
                    double lonExtent = maxLon - minLon;
                    double latExtent = maxLat - minLat;
                    double midLat = minLat + latExtent / 2.0;
                    double cosLat = std::cos(midLat * 3.14159265358979323846 / 180.0);
                    double xWidth = lonExtent * 2.0 * 3.14159265358979323846 * 6371000.0 * cosLat / 360.0;
                    double zWidth = latExtent * 2.0 * 3.14159265358979323846 * 6371000.0 / 360.0;

                    auto coordFunc = [&](double lat, double lon) -> std::pair<float, float> {
                        float x = (lonExtent > 0) ? static_cast<float>(((lon - minLon) * xWidth) / lonExtent) : 0.0f;
                        float z = (latExtent > 0) ? static_cast<float>(((lat - minLat) * zWidth) / latExtent) : 0.0f;
                        return {x, z};
                    };

                    static const size_t MAX_BUILDINGS = 5000;
                    static const size_t MAX_VERTICES = 200000;

                    size_t buildCount = 0;
                    size_t skippedWater = 0;
                    BuildingMesh batch;
                    for (size_t i = 0; i < footprints.size() && buildCount < MAX_BUILDINGS; i++) {
                        const auto& fp = footprints[i];
                        if (fp.outline.size() < 3) continue;

                        // Compute building centroid
                        double centLat = 0, centLon = 0;
                        for (const auto& [lat, lon] : fp.outline) {
                            centLat += lat; centLon += lon;
                        }
                        centLat /= fp.outline.size();
                        centLon /= fp.outline.size();

                        // Sample heightmap for ground Y and land/sea filtering.
                        // Uses the OSM-enhanced heightmap (not Natural Earth) so that
                        // buildings behind man-made features (e.g. Cardiff Bay Barrage)
                        // are correctly classified as on-land.
                        float gx = (lonExtent > 0) ? static_cast<float>((centLon - minLon) / lonExtent) : 0.5f;
                        float gz = (latExtent > 0) ? static_cast<float>((centLat - minLat) / latExtent) : 0.5f;
                        int hpx = std::max(0, std::min(resolution - 1, static_cast<int>(gx * (resolution - 1))));
                        int hpy = std::max(0, std::min(resolution - 1, static_cast<int>((1.0f - gz) * (resolution - 1))));
                        float groundY = heightGrid[hpy * resolution + hpx];

                        // Skip buildings in water (heightGrid < -5m after smoothing)
                        if (groundY < -5.0f) {
                            skippedWater++;
                            continue;
                        }
                        if (groundY < 0.0f) groundY = 0.0f;

                        BuildingMesh single = BuildingGenerator::generate(fp, coordFunc, groundY);
                        if (single.empty()) continue;
                        batch.append(single);
                        buildCount++;
                        if (batch.vertexCount() >= MAX_VERTICES) break;
                    }
                    // Note: breakwater/dam barriers are used for heightmap flood fill only.
                    // 3D harbour structures (piers, jetties, dams) come from OSMBuildingReader
                    // via the isStructure flag and are handled by the runtime building path.

                    if (!batch.empty()) {
                        std::ofstream f(outputDir + "/buildings.obj");
                        if (f.is_open()) f << batch.toOBJ("buildings", "building_wall", "building_roof");

                        // Simple hash for deterministic pseudo-random per-pixel variation
                        auto hash = [](int x, int y) -> uint32_t {
                            uint32_t h = (uint32_t)(x * 374761393 + y * 668265263);
                            h = (h ^ (h >> 13)) * 1274126177;
                            return h ^ (h >> 16);
                        };

                        // --- Wall texture (512x512): brick with windows ---
                        const int texW = 512, texH = 512;
                        std::vector<uint8_t> wallTex(texW * texH * 3);
                        {
                            const int brickH = 38, brickW = 17, mortarW = 2;
                            const int winW = 150, winH = 220;
                            const int winMarginX = (texW - 2 * winW) / 3;
                            const int winY0 = (texH - winH) / 2;
                            int win1X = winMarginX;
                            int win2X = winMarginX * 2 + winW;

                            auto isWindow = [&](int px, int py) -> bool {
                                return (px >= win1X && px < win1X + winW && py >= winY0 && py < winY0 + winH) ||
                                       (px >= win2X && px < win2X + winW && py >= winY0 && py < winY0 + winH);
                            };
                            auto isWindowFrame = [&](int px, int py) -> bool {
                                const int fw = 6;
                                for (int wx : {win1X, win2X}) {
                                    if (px >= wx - fw && px < wx + winW + fw &&
                                        py >= winY0 - fw && py < winY0 + winH + fw &&
                                        (px < wx || px >= wx + winW || py < winY0 || py >= winY0 + winH))
                                        return true;
                                }
                                return false;
                            };

                            for (int py = 0; py < texH; py++) {
                                for (int px = 0; px < texW; px++) {
                                    int idx = (py * texW + px) * 3;
                                    uint8_t r, g, b;
                                    if (isWindow(px, py)) {
                                        int v = 35 + (int)(hash(px/30, py/30) % 15);
                                        r = (uint8_t)v; g = (uint8_t)(v+8); b = (uint8_t)(v+20);
                                    } else if (isWindowFrame(px, py)) {
                                        r = g = b = (uint8_t)(185 + hash(px, py) % 10);
                                    } else {
                                        int row = py / brickH;
                                        int offX = (row % 2) ? brickW/2 : 0;
                                        int bx = (px + offX) % brickW, by = py % brickH;
                                        if (bx < mortarW || by < mortarW) {
                                            int mv = 175 + (int)(hash(px/3, py/3) % 12);
                                            r = (uint8_t)mv; g = (uint8_t)(mv-5); b = (uint8_t)(mv-10);
                                        } else {
                                            int brickId = row*100 + (px+offX)/brickW;
                                            int base = (int)(hash(brickId, 0) % 30);
                                            r = (uint8_t)(155+base); g = (uint8_t)(115+base*3/4); b = (uint8_t)(85+base/2);
                                            int noise = (int)(hash(px, py) % 8) - 4;
                                            r = (uint8_t)std::max(0, std::min(255, (int)r+noise));
                                            g = (uint8_t)std::max(0, std::min(255, (int)g+noise));
                                            b = (uint8_t)std::max(0, std::min(255, (int)b+noise));
                                        }
                                    }
                                    wallTex[idx] = r; wallTex[idx+1] = g; wallTex[idx+2] = b;
                                }
                            }
                        }
                        SatelliteTexture::writePNG(outputDir + "/building_wall.png", wallTex, texW, texH);

                        // --- Roof texture (256x256): dark slate/tile pattern ---
                        const int roofW = 256, roofH = 256;
                        std::vector<uint8_t> roofTex(roofW * roofH * 3);
                        {
                            const int tileH = 20, tileW = 40;
                            for (int py = 0; py < roofH; py++) {
                                for (int px = 0; px < roofW; px++) {
                                    int idx = (py * roofW + px) * 3;
                                    int row = py / tileH;
                                    int offX = (row % 2) ? tileW/2 : 0;
                                    int tx = (px + offX) % tileW;
                                    int ty = py % tileH;
                                    bool isEdge = (tx == 0 || ty == 0);

                                    // Slate grey base with per-tile variation
                                    int tileId = row * 50 + (px + offX) / tileW;
                                    int base = 75 + (int)(hash(tileId, 42) % 25);
                                    int noise = (int)(hash(px + 7777, py + 3333) % 6) - 3;

                                    uint8_t r, g, b;
                                    if (isEdge) {
                                        r = g = b = (uint8_t)(base - 15);
                                    } else {
                                        r = (uint8_t)std::max(0, std::min(255, base + noise));
                                        g = (uint8_t)std::max(0, std::min(255, base + noise - 3));
                                        b = (uint8_t)std::max(0, std::min(255, base + noise + 2));
                                    }
                                    roofTex[idx] = r; roofTex[idx+1] = g; roofTex[idx+2] = b;
                                }
                            }
                        }
                        SatelliteTexture::writePNG(outputDir + "/building_roof.png", roofTex, roofW, roofH);

                        // Write MTL with two materials
                        std::ofstream mtl(outputDir + "/buildings.mtl");
                        if (mtl.is_open()) {
                            mtl << "newmtl building_wall\n";
                            mtl << "Kd 0.9 0.9 0.9\n";
                            mtl << "Ka 0.1 0.1 0.1\n";
                            mtl << "Ks 0.03 0.03 0.03\n";
                            mtl << "Ns 10\n";
                            mtl << "d 1.0\n";
                            mtl << "Pr 0.75\n";
                            mtl << "Pm 0.0\n";
                            mtl << "map_Kd building_wall.png\n";
                            mtl << "\n";
                            mtl << "newmtl building_roof\n";
                            mtl << "Kd 0.6 0.6 0.6\n";
                            mtl << "Ka 0.05 0.05 0.05\n";
                            mtl << "Ks 0.02 0.02 0.02\n";
                            mtl << "Ns 5\n";
                            mtl << "d 1.0\n";
                            mtl << "Pr 0.85\n";
                            mtl << "Pm 0.0\n";
                            mtl << "map_Kd building_roof.png\n";
                        }
                    }
                    bldgReader.saveCache(outputDir + "/buildings_cache.dat");

                    resultMsg += (resultMsg.empty() ? "" : ", ") +
                        std::to_string(buildCount) + " buildings (" +
                        std::to_string(batch.vertexCount()) + " verts)";
                    if (skippedWater > 0)
                        resultMsg += " [" + std::to_string(skippedWater) + " in water skipped]";
                }
            }
        }

        // Download satellite texture
        if (wantSatellite) {
            generateStatus = "Downloading satellite tiles...";
            int zoom = SatelliteTexture::suggestZoom(minLat, maxLat, minLon, maxLon, resolution);
            int outW = 0, outH = 0;

            auto progressCb = [this](int ready, int total) -> bool {
                generateTilesReady = ready;
                generateTilesTotal = total;
                return true;
            };

            auto texData = SatelliteTexture::generate(
                minLat, maxLat, minLon, maxLon,
                zoom, resolution, cacheDir, outW, outH, progressCb);

            if (!texData.empty()) {
                SatelliteTexture::writePNG(outputDir + "/texture.png", texData, outW, outH);
                int mapW = 0, mapH = 0;
                auto mapData = SatelliteTexture::generate(
                    minLat, maxLat, minLon, maxLon,
                    std::max(1, zoom - 2), 512, cacheDir, mapW, mapH, nullptr);
                if (!mapData.empty()) {
                    SatelliteTexture::writePNG(outputDir + "/map.png", mapData, mapW, mapH);
                }
            } else {
                std::vector<uint8_t> fallback(resolution * resolution * 3);
                for (int i = 0; i < resolution * resolution; i++) {
                    fallback[i * 3 + 0] = 40;
                    fallback[i * 3 + 1] = 80;
                    fallback[i * 3 + 2] = 140;
                }
                SatelliteTexture::writePNG(outputDir + "/texture.png", fallback, resolution, resolution);
                SatelliteTexture::writePNG(outputDir + "/map.png", fallback, resolution, resolution);
                resultMsg += (resultMsg.empty() ? "" : "; ") + std::string("satellite download failed");
            }
        } else {
            std::vector<uint8_t> textureRGB(resolution * resolution * 3);
            for (int i = 0; i < resolution * resolution; i++) {
                textureRGB[i * 3 + 0] = 40;
                textureRGB[i * 3 + 1] = 80;
                textureRGB[i * 3 + 2] = 140;
            }
            SatelliteTexture::writePNG(outputDir + "/texture.png", textureRGB, resolution, resolution);
            SatelliteTexture::writePNG(outputDir + "/map.png", textureRGB, resolution, resolution);
        }

        if (resultMsg.empty()) {
            generateStatus = "World generated (no seamark data)";
        } else {
            generateStatus = "World generated: " + resultMsg;
        }
        isGenerating = false;
    }).detach();

    scenarioData.worldName = worldName;
    scenarioDirty = true;

    if (!isGenerating) {
        showGenerateDialog = false;
    }

    statusMessage = "World '" + worldName + "' created in " + outputDir;
}

void EditorApp::loadWorldData(const std::string& worldName) {
    if (worldName.empty() || worldName == loadedWorldName)
        return;

    // Find world directory
    std::string worldDir = "World/" + worldName;
    std::string userFolder = Utilities::getUserDir();
    if (Utilities::pathExists(userFolder + worldDir)) {
        worldDir = userFolder + worldDir;
    }

    buoyData = ScenarioFileIO::loadBuoys(worldDir);
    lightData = ScenarioFileIO::loadLights(worldDir);
    loadedWorldName = worldName;

    if (!buoyData.empty() || !lightData.empty()) {
        statusMessage = "Loaded " + std::to_string(buoyData.size()) + " buoys, "
                      + std::to_string(lightData.size()) + " lights from " + worldName;
    }
}

void EditorApp::loadChartFile() {
    if (!ChartOverlay::isGdalAvailable()) {
        statusMessage = "S-57 chart loading requires GDAL (build with -DWITH_GDAL=ON)";
        return;
    }

#ifdef _WIN32
    // Win32 file open dialog for .000 files (multi-select)
    // OFN_ALLOWMULTISELECT with OFN_EXPLORER returns: dir\0file1\0file2\0\0
    char fileBuf[4096] = {};
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = "S-57 Charts (*.000)\0*.000\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = sizeof(fileBuf);
    ofn.lpstrTitle = "Open S-57 Chart(s)";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR
              | OFN_ALLOWMULTISELECT | OFN_EXPLORER;

    if (GetOpenFileNameA(&ofn)) {
        // Parse multi-select result
        std::vector<std::string> chartFiles;
        const char* p = fileBuf;
        std::string dir = p;
        p += dir.size() + 1;

        if (*p == '\0') {
            // Single file selected (dir is the full path)
            chartFiles.push_back(dir);
        } else {
            // Multiple files: first string is directory, rest are filenames
            while (*p != '\0') {
                std::string fname = p;
                chartFiles.push_back(dir + "\\" + fname);
                p += fname.size() + 1;
            }
        }

        // If charts already loaded, add to existing; otherwise start fresh
        bool isFirst = !chartOverlay.isLoaded();
        int loaded = 0;
        for (const auto& path : chartFiles) {
            bool ok;
            if (isFirst) {
                ok = chartOverlay.loadChart(path);
                isFirst = false;
            } else {
                ok = chartOverlay.addChart(path);
            }
            if (ok) loaded++;
        }

        if (loaded > 0) {
            // Auto-zoom to combined chart extent
            double minLat, maxLat, minLon, maxLon;
            chartOverlay.getExtent(minLat, maxLat, minLon, maxLon);
            mapCenterLat = (minLat + maxLat) * 0.5;
            mapCenterLon = (minLon + maxLon) * 0.5;
            double latSpan = maxLat - minLat;
            if (latSpan > 5) mapZoom = 6;
            else if (latSpan > 2) mapZoom = 8;
            else if (latSpan > 1) mapZoom = 10;
            else if (latSpan > 0.5) mapZoom = 11;
            else if (latSpan > 0.1) mapZoom = 13;
            else mapZoom = 14;
            statusMessage = "Loaded " + std::to_string(loaded) + " chart(s), "
                          + std::to_string(chartOverlay.chartCount()) + " total";
        } else {
            statusMessage = "Failed to load chart file(s)";
        }
    }
#endif
}

void EditorApp::renderHelpOverlay() {
    ImGui::SetNextWindowPos(ImVec2((float)windowWidth * 0.5f, (float)windowHeight * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(420, 400), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.9f);

    if (ImGui::Begin("Keyboard Shortcuts", &showHelpOverlay,
                      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse)) {

        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "General");
        ImGui::Columns(2, nullptr, false);
        ImGui::SetColumnWidth(0, 140);
        ImGui::Text("Ctrl+N"); ImGui::NextColumn(); ImGui::Text("New scenario"); ImGui::NextColumn();
        ImGui::Text("Ctrl+O"); ImGui::NextColumn(); ImGui::Text("Open scenario"); ImGui::NextColumn();
        ImGui::Text("Ctrl+S"); ImGui::NextColumn(); ImGui::Text("Save scenario"); ImGui::NextColumn();
        ImGui::Text("Ctrl+Z"); ImGui::NextColumn(); ImGui::Text("Undo"); ImGui::NextColumn();
        ImGui::Text("Ctrl+Y"); ImGui::NextColumn(); ImGui::Text("Redo"); ImGui::NextColumn();
        ImGui::Text("F1"); ImGui::NextColumn(); ImGui::Text("Toggle this help"); ImGui::NextColumn();
        ImGui::Text("F5"); ImGui::NextColumn(); ImGui::Text("Test in simulator"); ImGui::NextColumn();
        ImGui::Text("Escape"); ImGui::NextColumn(); ImGui::Text("Cancel current tool"); ImGui::NextColumn();
        ImGui::Columns(1);

        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Map Navigation");
        ImGui::Columns(2, nullptr, false);
        ImGui::SetColumnWidth(0, 140);
        ImGui::Text("Arrow keys"); ImGui::NextColumn(); ImGui::Text("Pan map"); ImGui::NextColumn();
        ImGui::Text("+/-"); ImGui::NextColumn(); ImGui::Text("Zoom in/out"); ImGui::NextColumn();
        ImGui::Text("Scroll wheel"); ImGui::NextColumn(); ImGui::Text("Zoom in/out"); ImGui::NextColumn();
        ImGui::Text("Right-drag"); ImGui::NextColumn(); ImGui::Text("Pan map"); ImGui::NextColumn();
        ImGui::Text("Home"); ImGui::NextColumn(); ImGui::Text("Centre on own ship"); ImGui::NextColumn();
        ImGui::Columns(1);

        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Tools");
        ImGui::Columns(2, nullptr, false);
        ImGui::SetColumnWidth(0, 140);
        ImGui::Text("S"); ImGui::NextColumn(); ImGui::Text("Select tool"); ImGui::NextColumn();
        ImGui::Text("O"); ImGui::NextColumn(); ImGui::Text("Place own ship"); ImGui::NextColumn();
        ImGui::Text("N"); ImGui::NextColumn(); ImGui::Text("Place new ship"); ImGui::NextColumn();
        ImGui::Text("W"); ImGui::NextColumn(); ImGui::Text("Add waypoint"); ImGui::NextColumn();
        ImGui::Text("A"); ImGui::NextColumn(); ImGui::Text("Draw area"); ImGui::NextColumn();
        ImGui::Text("R"); ImGui::NextColumn(); ImGui::Text("Measure distance"); ImGui::NextColumn();
        ImGui::Text("C"); ImGui::NextColumn(); ImGui::Text("Load S-57 chart"); ImGui::NextColumn();
        ImGui::Text("M"); ImGui::NextColumn(); ImGui::Text("Toggle satellite/street map"); ImGui::NextColumn();
        ImGui::Text("K"); ImGui::NextColumn(); ImGui::Text("Toggle seamark overlay"); ImGui::NextColumn();
        ImGui::Text("B"); ImGui::NextColumn(); ImGui::Text("Toggle building footprints"); ImGui::NextColumn();
        ImGui::Text("Delete"); ImGui::NextColumn(); ImGui::Text("Delete selected ship"); ImGui::NextColumn();
        ImGui::Columns(1);
    }
    ImGui::End();
}

void EditorApp::renderOpenDialog() {
    if (!showOpenDialog) return;

    ImGui::OpenPopup("Open Scenario");
    ImVec2 center = ImVec2((float)windowWidth * 0.5f, (float)windowHeight * 0.5f);
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(400, 400), ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal("Open Scenario", &showOpenDialog)) {
        ImGui::Text("Select a scenario to open:");
        ImGui::Separator();

        ImGui::BeginChild("ScenarioList", ImVec2(0, -30), true);
        for (int i = 0; i < (int)availableScenarios.size(); i++) {
            bool selected = (i == selectedScenarioIdx);
            if (ImGui::Selectable(availableScenarios[i].c_str(), selected)) {
                selectedScenarioIdx = i;
            }
            if (selected && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                openScenario(availableScenarios[i]);
                showOpenDialog = false;
            }
        }
        ImGui::EndChild();

        if (ImGui::Button("Open", ImVec2(120, 0))) {
            if (selectedScenarioIdx >= 0 && selectedScenarioIdx < (int)availableScenarios.size()) {
                openScenario(availableScenarios[selectedScenarioIdx]);
                showOpenDialog = false;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            showOpenDialog = false;
        }
        ImGui::EndPopup();
    }
}

void EditorApp::renderSaveAsDialog() {
    if (!showSaveAsDialog) return;

    ImGui::OpenPopup("Save Scenario As");
    ImVec2 center = ImVec2((float)windowWidth * 0.5f, (float)windowHeight * 0.5f);
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(400, 120), ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal("Save Scenario As", &showSaveAsDialog)) {
        ImGui::Text("Scenario name:");
        bool enterPressed = ImGui::InputText("##saveas", saveAsNameBuf, sizeof(saveAsNameBuf),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::Button("Save", ImVec2(120, 0)) || enterPressed) {
            std::string name(saveAsNameBuf);
            if (!name.empty()) {
                saveScenarioAs(name);
                showSaveAsDialog = false;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            showSaveAsDialog = false;
        }
        ImGui::EndPopup();
    }
}
