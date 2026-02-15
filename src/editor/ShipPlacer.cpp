#include "ShipPlacer.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "../graphics/wicked/imgui/imgui.h"
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

bool ShipPlacer::handleMapClick(double lat, double lon, ScenarioData& scenario) {
    switch (currentTool) {
    case PLACE_OWNSHIP:
        scenario.ownShipData.initialLat = (float)lat;
        scenario.ownShipData.initialLong = (float)lon;
        selectedShip = -1;
        currentTool = SELECT;
        return true;

    case PLACE_OTHERSHIP: {
        OtherShipData ship;
        ship.initialLat = (float)lat;
        ship.initialLong = (float)lon;
        ship.shipName = "Cargoship1";
        ship.mmsi = 0;
        ship.drifting = false;
        // Add a default stop leg
        LegData stopLeg;
        stopLeg.bearing = 0;
        stopLeg.speed = 0;
        stopLeg.distance = 0;
        stopLeg.startTime = 0;
        ship.legs.push_back(stopLeg);
        scenario.otherShipsData.push_back(std::move(ship));
        selectedShip = (int)scenario.otherShipsData.size() - 1;
        // Stay in PLACE_WAYPOINT mode to add waypoints
        currentTool = PLACE_WAYPOINT;
        return true;
    }

    case PLACE_WAYPOINT:
        if (selectedShip >= 0 && selectedShip < (int)scenario.otherShipsData.size()) {
            auto& ship = scenario.otherShipsData[selectedShip];
            // Calculate the actual end position by following all existing legs
            float lastLat = ship.initialLat;
            float lastLon = ship.initialLong;
            for (size_t li = 0; li < ship.legs.size(); li++) {
                const auto& l = ship.legs[li];
                if (l.speed == 0 && l.distance == 0) continue; // skip stop legs
                float bearRad = l.bearing * (float)M_PI / 180.0f;
                float distDeg = l.distance / 60.0f;
                lastLat += distDeg * std::cos(bearRad);
                lastLon += distDeg * std::sin(bearRad) /
                           std::cos(lastLat * (float)M_PI / 180.0f);
            }

            // Calculate bearing from last position to click point
            double dLat = lat - lastLat;
            double dLon = lon - lastLon;
            double bearing = std::atan2(dLon * std::cos(lastLat * M_PI / 180.0), dLat) * 180.0 / M_PI;
            if (bearing < 0) bearing += 360.0;

            // Calculate distance in nautical miles
            double latDiff = lat - lastLat;
            double lonDiff = (lon - lastLon) * std::cos(((lat + lastLat) / 2.0) * M_PI / 180.0);
            double distDeg = std::sqrt(latDiff * latDiff + lonDiff * lonDiff);
            double distNm = distDeg * 60.0; // 1 degree latitude ~ 60 NM

            // Insert before the stop leg (which is always last)
            LegData leg;
            leg.bearing = (float)bearing;
            leg.speed = 10.0f; // default 10 knots
            leg.distance = (float)distNm;
            leg.startTime = 0;

            if (!ship.legs.empty()) {
                // Insert before the last (stop) leg
                ship.legs.insert(ship.legs.end() - 1, leg);
            } else {
                ship.legs.push_back(leg);
            }
            return true;
        }
        currentTool = SELECT;
        return false;

    case SELECT:
        // TODO: Hit-test against ship icons to select
        return false;
    }
    return false;
}

void ShipPlacer::drawShipIcon(ImDrawList* drawList, float x, float y,
                               float heading, unsigned int color,
                               bool isSelected, const char* label) {
    // Draw a triangle pointing in the heading direction
    float size = 10.0f;
    float rad = heading * (float)M_PI / 180.0f;

    // Triangle vertices: tip, left, right
    float tipX = x + std::sin(rad) * size;
    float tipY = y - std::cos(rad) * size;
    float leftX = x + std::sin(rad - 2.5f) * size * 0.6f;
    float leftY = y - std::cos(rad - 2.5f) * size * 0.6f;
    float rightX = x + std::sin(rad + 2.5f) * size * 0.6f;
    float rightY = y - std::cos(rad + 2.5f) * size * 0.6f;

    drawList->AddTriangleFilled(
        ImVec2(tipX, tipY), ImVec2(leftX, leftY), ImVec2(rightX, rightY), color);

    // Course line extending from ship along heading
    float courseLen = 30.0f;
    float endX = x + std::sin(rad) * courseLen;
    float endY = y - std::cos(rad) * courseLen;
    drawList->AddLine(ImVec2(tipX, tipY), ImVec2(endX, endY),
                      (color & 0x00FFFFFF) | 0x60000000, 1.0f); // Same color, reduced alpha

    if (isSelected) {
        drawList->AddCircle(ImVec2(x, y), size + 4, IM_COL32(255, 255, 0, 200), 16, 2.0f);
    }

    if (label) {
        drawList->AddText(ImVec2(x + size + 4, y - 6), IM_COL32(255, 255, 255, 200), label);
    }
}

void ShipPlacer::drawWaypointLines(ImDrawList* drawList,
                                    float shipX, float shipY,
                                    const OtherShipData& ship,
                                    LatLonToPixelFn toPixel, void* userData,
                                    unsigned int color) {
    if (ship.legs.empty())
        return;

    // Compute waypoint positions by following legs from initial position
    float curLat = ship.initialLat;
    float curLon = ship.initialLong;
    ImVec2 prevPos = ImVec2(shipX, shipY);

    for (size_t i = 0; i < ship.legs.size(); i++) {
        const auto& leg = ship.legs[i];
        if (leg.speed == 0 && leg.distance == 0)
            continue; // Skip stop legs

        // Compute next position from bearing and distance
        float bearRad = leg.bearing * (float)M_PI / 180.0f;
        float distDeg = leg.distance / 60.0f; // NM to degrees latitude approx
        float nextLat = curLat + distDeg * std::cos(bearRad);
        float nextLon = curLon + distDeg * std::sin(bearRad) /
                        std::cos(curLat * (float)M_PI / 180.0f);

        ImVec2 nextPos = toPixel(nextLat, nextLon, userData);

        // Draw leg line
        drawList->AddLine(prevPos, nextPos, color, 1.5f);

        // Draw waypoint dot
        drawList->AddCircleFilled(nextPos, 4.0f, color);
        drawList->AddCircle(nextPos, 4.0f, IM_COL32(255, 255, 255, 180), 8, 1.0f);

        // Waypoint number
        char wpLabel[16];
        snprintf(wpLabel, sizeof(wpLabel), "%zu", i + 1);
        ImVec2 wpSize = ImGui::CalcTextSize(wpLabel);
        drawList->AddText(ImVec2(nextPos.x - wpSize.x * 0.5f, nextPos.y + 5),
                          IM_COL32(255, 255, 255, 200), wpLabel);

        // Speed label at midpoint
        char speedLabel[32];
        snprintf(speedLabel, sizeof(speedLabel), "%.0f kts", leg.speed);
        ImVec2 midPt = ImVec2((prevPos.x + nextPos.x) * 0.5f, (prevPos.y + nextPos.y) * 0.5f);
        drawList->AddText(ImVec2(midPt.x + 4, midPt.y - 10),
                          IM_COL32(255, 255, 255, 160), speedLabel);

        curLat = nextLat;
        curLon = nextLon;
        prevPos = nextPos;
    }
}

void ShipPlacer::renderShips(ImDrawList* drawList,
                              const ScenarioData& scenario,
                              LatLonToPixelFn toPixel, void* userData) {
    // Render own ship
    if (scenario.ownShipData.initialLat != 0 || scenario.ownShipData.initialLong != 0) {
        ImVec2 pos = toPixel(scenario.ownShipData.initialLat,
                              scenario.ownShipData.initialLong, userData);
        drawShipIcon(drawList, pos.x, pos.y,
                     scenario.ownShipData.initialBearing,
                     IM_COL32(50, 255, 50, 255),
                     selectedShip == -1, "Own Ship");
    }

    // Render other ships
    for (size_t i = 0; i < scenario.otherShipsData.size(); i++) {
        const auto& ship = scenario.otherShipsData[i];
        ImVec2 pos = toPixel(ship.initialLat, ship.initialLong, userData);

        bool isSelected = (selectedShip == (int)i);
        unsigned int shipColor = isSelected ? IM_COL32(255, 100, 100, 255)
                                            : IM_COL32(255, 50, 50, 255);

        char label[128];
        if (!ship.shipName.empty() && ship.shipName != "Ship")
            snprintf(label, sizeof(label), "%zu: %s", i + 1, ship.shipName.c_str());
        else
            snprintf(label, sizeof(label), "Ship %zu", i + 1);
        drawShipIcon(drawList, pos.x, pos.y, 0, shipColor, isSelected, label);

        // Draw waypoint lines
        unsigned int lineColor = isSelected ? IM_COL32(255, 200, 50, 200)
                                            : IM_COL32(255, 100, 100, 100);
        drawWaypointLines(drawList, pos.x, pos.y, ship, toPixel, userData, lineColor);
    }
}
