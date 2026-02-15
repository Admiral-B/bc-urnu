#pragma once

#include <cmath>
#include <algorithm>

// Lets users draw a rectangle on the map to define the scenario area.
// The rectangle determines the world bounds (terrain.ini TerrainLong/Lat/Extent).
class AreaSelector {
public:
    enum State { IDLE, DRAWING, COMPLETE };

    void startDrawing() { state = DRAWING; }
    void clear() { state = IDLE; hasFirstCorner = false; }

    // Handle a map click at the given lat/lon. Returns true if the state changed.
    bool handleClick(double lat, double lon);

    // Update the second corner while drawing (for live preview)
    void updatePreview(double lat, double lon);

    State getState() const { return state; }

    double getMinLat() const { return std::min(corner1Lat, corner2Lat); }
    double getMaxLat() const { return std::max(corner1Lat, corner2Lat); }
    double getMinLon() const { return std::min(corner1Lon, corner2Lon); }
    double getMaxLon() const { return std::max(corner1Lon, corner2Lon); }

    // Estimated world dimensions
    double getWidthKm() const;   // East-west extent in km
    double getHeightKm() const;  // North-south extent in km

private:
    State state = IDLE;
    bool hasFirstCorner = false;
    double corner1Lat = 0, corner1Lon = 0;
    double corner2Lat = 0, corner2Lon = 0;
};
