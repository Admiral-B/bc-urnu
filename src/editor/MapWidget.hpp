#pragma once

#include "TileMath.hpp"
#include <string>
#include <functional>

namespace irr {
    namespace video { class IVideoDriver; class ITexture; }
    namespace gui { class IGUIEnvironment; class IGUIFont; }
    struct SEvent;
}

class TileDownloader;
class TileTextureManager;

// Map widget that displays a pannable, zoomable slippy map using Irrlicht 2D rendering.
// Renders satellite/street tile imagery with pan (drag) and zoom (scroll) controls.
class MapWidget {
public:
    MapWidget(TileDownloader* downloader, TileTextureManager* texManager);
    ~MapWidget();

    // Render the map into the given screen rectangle. Call every frame.
    void render(irr::video::IVideoDriver* driver, irr::gui::IGUIFont* font,
                int screenX, int screenY, int screenW, int screenH);

    // Handle input events. Returns true if the event was consumed.
    bool onEvent(const irr::SEvent& event, int screenX, int screenY, int screenW, int screenH);

    // Map state
    void setCenter(double lat, double lon);
    void setZoom(int zoom); // 0-19
    double getCenterLat() const { return centerLat; }
    double getCenterLon() const { return centerLon; }
    int getZoom() const { return zoom; }

    // Convert between screen coords (within widget) and lat/lon
    TileMath::LatLon screenToLatLon(int screenX, int screenY) const;
    TileMath::PixelPos latLonToScreen(double lat, double lon) const;

    // Callbacks
    std::function<void(double lat, double lon, int button)> onMapClick;
    std::function<void(double lat, double lon)> onMapHover;

    // Layer toggles
    bool showGrid = true;
    bool showOverlay = true;

    // Mouse position in lat/lon (updated every frame)
    double mouseLat = 0.0;
    double mouseLon = 0.0;

private:
    TileDownloader* downloader;
    TileTextureManager* texManager;

    double centerLat = 50.0;
    double centerLon = -5.0;
    int zoom = 6;

    // Dragging state
    bool isDragging = false;
    int dragStartMouseX = 0, dragStartMouseY = 0;
    double dragStartLat = 0.0, dragStartLon = 0.0;

    // Widget area (set during render)
    int widgetX = 0, widgetY = 0, widgetW = 0, widgetH = 0;

    // Last known mouse position within widget
    int lastMouseX = 0, lastMouseY = 0;

    void renderTiles(irr::video::IVideoDriver* driver);
    void renderGridLines(irr::video::IVideoDriver* driver, irr::gui::IGUIFont* font);
    void renderOverlay(irr::video::IVideoDriver* driver, irr::gui::IGUIFont* font);
};
