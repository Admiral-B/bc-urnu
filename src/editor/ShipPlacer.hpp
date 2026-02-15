#pragma once

#include "../ScenarioDataStructure.hpp"

struct ImDrawList;
struct ImVec2;

// Handles ship placement and rendering on the ImGui map.
class ShipPlacer {
public:
    enum Tool { SELECT, PLACE_OWNSHIP, PLACE_OTHERSHIP, PLACE_WAYPOINT };

    Tool currentTool = SELECT;

    // Handle map click. Returns true if something was placed/selected.
    bool handleMapClick(double lat, double lon, ScenarioData& scenario);

    // Select a ship by index (-1 = own ship, 0+ = other ships)
    void selectShip(int index) { selectedShip = index; }
    int getSelectedShip() const { return selectedShip; }

    // Render all ship icons and waypoint lines.
    // latLonToPixelFn converts (lat, lon) -> screen (x, y)
    using LatLonToPixelFn = ImVec2(*)(double lat, double lon, void* userData);
    void renderShips(ImDrawList* drawList,
                     const ScenarioData& scenario,
                     LatLonToPixelFn toPixel, void* userData);

private:
    int selectedShip = -2; // -2 = none, -1 = own ship, 0+ = other ships

    void drawShipIcon(ImDrawList* drawList, float x, float y,
                      float heading, unsigned int color, bool isSelected, const char* label);
    void drawWaypointLines(ImDrawList* drawList,
                           float shipX, float shipY,
                           const OtherShipData& ship,
                           LatLonToPixelFn toPixel, void* userData,
                           unsigned int color);
};
