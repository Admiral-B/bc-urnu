#pragma once

// New Dear ImGui-based scenario editor application.
// Replaces the Irrlicht-based editor GUI with a modern ImGui interface.
// Uses Win32 + OpenGL3 backends on Windows.

#include <string>
#include <memory>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "LocationSearch.hpp"
#include "AreaSelector.hpp"
#include "ShipPlacer.hpp"
#include "ScenarioFileIO.hpp"
#include "ChartOverlay.hpp"
#include "MeasureTool.hpp"
#include "UndoStack.hpp"
#include "TileDownloader.hpp"
#include "OSMBuildingReader.hpp"
#include "../ScenarioDataStructure.hpp"
#include <unordered_map>

class EditorApp {
public:
    bool init(int width, int height, const std::string& title);
    void run();   // Main loop (blocks until window closes)
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

    // Map state
    double mapCenterLat = 50.0;
    double mapCenterLon = -5.0;
    int mapZoom = 6;

    // Tile rendering
    enum TileSource { TILE_SATELLITE, TILE_STREET };
    TileSource tileSource = TILE_SATELLITE;
    std::unique_ptr<TileDownloader> satelliteDownloader;
    std::unique_ptr<TileDownloader> streetDownloader;
    std::unique_ptr<TileDownloader> seamarkDownloader;
    bool seamarkOverlay = false;
    unsigned int glPlaceholderTex = 0; // 1x1 dark blue texture for missing tiles

    // GL texture cache: "z/x/y" -> OpenGL texture ID
    std::unordered_map<std::string, unsigned int> tileTexCache;
    std::vector<std::string> texCacheOrder; // LRU eviction order
    static const size_t MAX_TEX_CACHE = 300;
    unsigned int getOrCreateTileTexture(TileDownloader* dl, int z, int x, int y);
    void evictOldTextures();

    // Search state
    LocationSearch locationSearch;
    char searchBuf[256] = {};
    std::vector<LocationSearch::Result> searchResults;
    bool showSearchResults = false;

    // Area selection
    AreaSelector areaSelector;

    // Ship placement and scenario data
    ShipPlacer shipPlacer;
    ScenarioData scenarioData;

    // Mouse position in lat/lon (approximated from map panel pixel position)
    double mouseLat = 0.0;
    double mouseLon = 0.0;

    // Ship drag state
    bool isDraggingShip = false;
    ScenarioData dragBeforeState;

    // Right-click context menu state
    double contextMenuLat = 0.0;
    double contextMenuLon = 0.0;
    int contextMenuHitShip = -2;  // -2 = water, -1 = own ship, 0+ = other ship
    int contextMenuHitLeg = -1;   // -1 = none, 0+ = leg index
    int contextMenuLegShip = -1;  // which ship the hit leg belongs to

    // Scenario file I/O
    std::string scenarioPath; // Full path to current scenario directory (empty if unsaved)
    bool scenarioDirty = false;
    bool showOpenDialog = false;
    bool showSaveAsDialog = false;
    std::vector<std::string> availableScenarios;
    std::vector<std::string> availableWorlds;
    int selectedScenarioIdx = -1;
    char saveAsNameBuf[128] = {};
    std::string scenarioBaseDir; // e.g. "Scenarios/" or user dir + "Scenarios/"
    std::string statusMessage; // Shown in status bar

    // Available ship models (populated from Models/ directories)
    std::vector<std::string> otherShipModels;
    std::vector<std::string> ownShipModels;

    // World data (buoys and lights)
    std::vector<EditorBuoyData> buoyData;
    std::vector<EditorLightData> lightData;
    std::string loadedWorldName; // Track which world's data is loaded
    void loadWorldData(const std::string& worldName);

    // Undo/redo
    UndoStack undoStack;

    // Help overlay
    bool showHelpOverlay = false;
    void renderHelpOverlay();

    // Measurement tool
    MeasureTool measureTool;

    // Chart overlay (S-57)
    ChartOverlay chartOverlay;
    void loadChartFile(); // Opens file dialog and loads S-57 chart

    // Building footprint overlay (OSM)
    bool showBuildings = false;
    OSMBuildingReader buildingReader;
    bool buildingsQueried = false;  // true if we've queried for current view area
    double buildingsQueryLat = 0, buildingsQueryLon = 0;
    int buildingsQueryZoom = 0;

    // World generation dialog
    bool showGenerateDialog = false;
    char worldNameBuf[128] = {};
    int worldResolution = 2049;
    bool useGEBCO = true;
    bool useCopernicusDEM = true;
    bool useSatelliteTexture = true;
    bool useOpenSeaMap = true;
    bool isGenerating = false;
    int generateTilesReady = 0;
    int generateTilesTotal = 0;
    std::string generateStatus;

    // Platform helpers
    bool createWindow(int width, int height, const std::string& title);
    bool createGLContext();
    void destroyGLContext();

    // Search
    void performSearch();

    // File operations
    void newScenario();
    void openScenario(const std::string& name);
    bool saveScenario();
    bool saveScenarioAs(const std::string& name);
    void refreshScenarioList();
    void testInSimulator();
    void exportScenarioZip(bool includeWorld);
    void importScenarioZip();
    void generateWorldFromArea();

    // ImGui frame rendering
    void renderFrame();
    void renderMenuBar();
    void renderToolbar(float topY);
    void renderMapPanel(float x, float y, float w, float h);
    void renderPropertiesPanel(float x, float y, float w, float h);
    void renderStatusBar(float topY);
    void renderGenerateWorldDialog();
    void renderOpenDialog();
    void renderSaveAsDialog();

    // Win32 message handler
#ifdef _WIN32
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
#endif
};
