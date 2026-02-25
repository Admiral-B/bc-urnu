/*   Bridge Command 5.0 Ship Simulator
     Copyright (C) 2024 James Packer

     This program is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License version 2 as
     published by the Free Software Foundation */

#ifndef BC_GUI_ECDIS_DISPLAY_HPP
#define BC_GUI_ECDIS_DISPLAY_HPP

#ifdef WITH_WICKED_ENGINE

#include <cstdint>
#include <memory>
#include <vector>
#include <string>
#include <cmath>

#ifndef ImTextureID
typedef void* ImTextureID;
#endif

class TileDownloader;

namespace bc { namespace gui {

class EcdisDisplay {
public:
    EcdisDisplay();
    ~EcdisDisplay();

    void init(const std::string& cacheDir);

    struct OwnShipData {
        double lat = 0, lon = 0;
        float heading = 0;       // degrees true
        float cog = 0;           // degrees true
        float sog = 0;           // knots
        float stw = 0;           // knots
        float depth = 0;         // metres
        float windSpeed = 0;     // knots
        float windDirection = 0; // degrees true
        float rudder = 0;        // degrees
        float tidalStreamX = 0;  // m/s east
        float tidalStreamZ = 0;  // m/s north
        float simulationTime = 0;
    };

    struct AISTarget {
        double lat = 0, lon = 0;
        float heading = 0;
        float cog = 0;
        float speed = 0; // SOG knots
        int id = 0;
        uint32_t mmsi = 0;
        std::string name;
        float length = 0, breadth = 0;
    };

    void setOwnShipData(const OwnShipData& data);
    void setAISTargets(const std::vector<AISTarget>& targets);

    // Render full-screen ECDIS. Returns false if user closed it.
    bool render(int screenWidth, int screenHeight);

private:
    // Tile downloaders
    std::unique_ptr<TileDownloader> osmDownloader_;
    std::unique_ptr<TileDownloader> seamarkDownloader_;
    bool initialized_ = false;

    // Chart view state
    double centerLat_ = 0, centerLon_ = 0;
    int zoom_ = 14;
    bool centerOnShip_ = true;

    // Data
    OwnShipData ownShip_;
    std::vector<AISTarget> aisTargets_;

    // Chart area geometry (set each frame -- full screen minus status bar)
    float chartX_ = 0, chartY_ = 0, chartW_ = 0, chartH_ = 0;

    // GPU tile cache (opaque)
    struct GPUTileCacheImpl;
    std::unique_ptr<GPUTileCacheImpl> tileCache_;
    int tileCacheZoom_ = -1;
    static constexpr size_t MAX_GPU_TILES = 200;

    // Interaction
    bool isDragging_ = false;
    double dragStartLat_ = 0, dragStartLon_ = 0;
    bool floatingWindowHovered_ = false;  // set by floating window render methods

    // Display options
    int paletteIndex_ = 1; // 0=Day, 1=Dusk, 2=Night
    bool showDataBox_ = true;
    bool closeRequested_ = false;

    // Track history (own ship trail)
    struct TrackPoint { double lat, lon; float heading, time; };
    std::vector<TrackPoint> trackHistory_;
    float lastTrackSampleTime_ = -999;
    static constexpr float TRACK_SAMPLE_INTERVAL = 3.0f; // seconds
    static constexpr size_t MAX_TRACK_POINTS = 3000;

    // Route planning
    struct Waypoint { double lat, lon; };
    std::vector<Waypoint> routeWaypoints_;
    enum class InteractionMode { Navigate, AddWaypoint, MeasureBearingDistance, PlaceMarker };
    InteractionMode interactionMode_ = InteractionMode::Navigate;
    int selectedWaypointIdx_ = -1;
    int dragWaypointIdx_ = -1;

    // AIS interrogation
    int selectedAISIdx_ = -1; // index into aisTargets_

    // DR/EP projection
    bool showDREP_ = true;
    float drepMinutes_ = 30.0f;

    // Measurement tool
    struct MeasurementLine { double lat1, lon1, lat2, lon2; };
    std::vector<MeasurementLine> measurements_;
    int activeMeasureStep_ = 0; // 0=ready, 1=first point placed
    double measureLat1_ = 0, measureLon1_ = 0;

    // Chart markers
    struct ChartMarker { double lat, lon; std::string label; };
    std::vector<ChartMarker> chartMarkers_;
    int selectedMarkerIdx_ = -1;

    // Rendering submethods
    void renderTiles(void* drawList);
    void renderTrackHistory(void* drawList);
    void renderRoute(void* drawList);
    void renderOwnShip(void* drawList);
    void renderAISTargets(void* drawList);
    void renderAISInfoPopup();
    void renderDREP(void* drawList);
    void renderMeasurements(void* drawList);
    void renderMarkers(void* drawList);
    void renderCompassRose(void* drawList, float cx, float cy, float radius);
    void renderScaleBar(void* drawList, float x, float y);
    void renderDataBox(float screenW);
    void renderToolbar(float screenH);
    void renderStatusBar(float x, float y, float w, float h);

    // Coordinate transforms
    struct ScreenPos { float x, y; };
    ScreenPos latLonToScreen(double lat, double lon) const;
    struct LatLonPos { double lat, lon; };
    LatLonPos screenToLatLon(float sx, float sy) const;

    // Tile texture management
    ImTextureID getOrCreateTileTexture(int z, int x, int y);
    void evictOldTiles();
    void clearTileCache();

    // Zoom with cache management
    void setZoom(int newZoom);

    // Helpers
    float metersPerPixel() const;
    float nmToPixels(float nm) const;
    uint32_t tileTint() const;

    static void calcBearingDistance(double lat1, double lon1,
                                     double lat2, double lon2,
                                     float& bearingDeg, float& distanceNm);
};

}} // namespace bc::gui

#endif // WITH_WICKED_ENGINE
#endif // BC_GUI_ECDIS_DISPLAY_HPP
