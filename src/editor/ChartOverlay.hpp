#pragma once

#include <string>
#include <vector>
#include <cstdint>

// Forward declare ImDrawList to avoid pulling in imgui.h
struct ImDrawList;
struct ImVec2;

// Lightweight chart feature structs for rendering (no GDAL dependency)
struct OverlayDepthArea {
    double minDepth = 0;
    double maxDepth = 0;
    struct Point { float x, y; }; // Stored as lat/lon
    std::vector<Point> boundary;
};

struct OverlaySounding {
    float lat = 0, lon = 0;
    float depth = 0;
};

struct OverlayCoastline {
    struct Point { float x, y; }; // lat/lon
    std::vector<Point> points;
};

struct OverlayBuoy {
    float lat = 0, lon = 0;
    std::string name;
    std::string layerName;
    uint8_t r = 255, g = 200, b = 0; // Display colour
};

struct OverlayLight {
    float lat = 0, lon = 0;
    uint8_t r = 255, g = 255, b = 255;
    float range = 5.0f;
};

struct OverlayLandmark {
    float lat = 0, lon = 0;
    std::string name;
    int category = 0;
};

struct OverlayTSSArea {
    std::string layerName; // TSSLPT, TSSRON, TSEZNE
    struct Point { float x, y; }; // lat/lon
    std::vector<Point> boundary;
};

// Callback type for lat/lon -> pixel conversion
using OverlayToPixelFn = ImVec2(*)(double lat, double lon, void* userData);

class ChartOverlay {
public:
    // Load S-57 chart file (.000). Replaces all existing data.
    bool loadChart(const std::string& chartPath);

    // Add an additional S-57 chart, merging features into existing data.
    bool addChart(const std::string& chartPath);

    // Clear all loaded chart data
    void clear();

    // Is a chart loaded?
    bool isLoaded() const { return chartLoaded; }

    // How many charts are loaded?
    int chartCount() const { return (int)loadedPaths.size(); }

    // Is GDAL available in this build?
    static bool isGdalAvailable();

    // Get geographic extent of all loaded charts
    void getExtent(double& minLat, double& maxLat, double& minLon, double& maxLon) const;

    // Render chart features onto map
    void render(ImDrawList* drawList, int zoom,
                OverlayToPixelFn toPixel, void* userData,
                float mapX, float mapY, float mapW, float mapH);

    // Get loaded chart paths
    const std::vector<std::string>& getChartPaths() const { return loadedPaths; }

private:
    bool chartLoaded = false;
    std::string loadedPath; // Legacy single-chart path
    std::vector<std::string> loadedPaths;

    // Geographic extent
    double extMinLat = 0, extMaxLat = 0;
    double extMinLon = 0, extMaxLon = 0;

    // Feature data
    std::vector<OverlayDepthArea> depthAreas;
    std::vector<OverlaySounding> soundings;
    std::vector<OverlayCoastline> coastlines;
    std::vector<OverlayBuoy> buoys;
    std::vector<OverlayLight> lights;
    std::vector<OverlayLandmark> landmarks;
    std::vector<OverlayTSSArea> tssAreas;

    // Rendering helpers
    void renderDepthAreas(ImDrawList* drawList, int zoom,
                          OverlayToPixelFn toPixel, void* userData);
    void renderSoundings(ImDrawList* drawList, int zoom,
                         OverlayToPixelFn toPixel, void* userData,
                         float mapX, float mapY, float mapW, float mapH);
    void renderCoastlines(ImDrawList* drawList, int zoom,
                          OverlayToPixelFn toPixel, void* userData);
    void renderBuoys(ImDrawList* drawList, int zoom,
                     OverlayToPixelFn toPixel, void* userData,
                     float mapX, float mapY, float mapW, float mapH);
    void renderLights(ImDrawList* drawList, int zoom,
                      OverlayToPixelFn toPixel, void* userData,
                      float mapX, float mapY, float mapW, float mapH);
    void renderLandmarks(ImDrawList* drawList, int zoom,
                         OverlayToPixelFn toPixel, void* userData,
                         float mapX, float mapY, float mapW, float mapH);
    void renderTSSAreas(ImDrawList* drawList, int zoom,
                        OverlayToPixelFn toPixel, void* userData);

    // Depth to colour mapping for depth areas
    static unsigned int depthToColor(double minDepth, double maxDepth);
};
