/*   Bridge Command 5.0 Ship Simulator
     Copyright (C) 2024 James Packer

     This program is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License version 2 as
     published by the Free Software Foundation */

#ifdef WITH_WICKED_ENGINE

#include "RadarDisplay.hpp"
#include "../graphics/wicked/imgui/imgui.h"
#include "../SimulationBridge.hpp"
#include <cstring>
#include <algorithm>
#include <cmath>

namespace bc { namespace gui {

RadarDisplay::RadarDisplay() {}
RadarDisplay::~RadarDisplay() {}

bool RadarDisplay::render(int screenWidth, int screenHeight,
                          ImTextureID radarTexID, int radarSize) {
    bool open = true;

    // Full-screen background window
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2((float)screenWidth, (float)screenHeight));
    ImGuiWindowFlags bgFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoNav;

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.02f, 0.05f, 0.02f, 1.0f));
    ImGui::Begin("##RadarFullscreen", nullptr, bgFlags);
    ImGui::PopStyleColor();

    // Layout: radar PPI on the left, controls on the right
    float controlPanelW = std::min(300.0f, screenWidth * 0.25f);
    float radarAreaW = screenWidth - controlPanelW;
    float radarAreaH = (float)screenHeight;

    // Draw radar image as a square centered in the left area
    float radarDispSize = std::min(radarAreaW - 20.0f, radarAreaH - 20.0f);
    float radarX = (radarAreaW - radarDispSize) * 0.5f;
    float radarY = (radarAreaH - radarDispSize) * 0.5f;

    if (radarTexID && radarSize > 0 && radarOn_) {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec2 pMin = ImVec2(radarX, radarY);
        ImVec2 pMax = ImVec2(radarX + radarDispSize, radarY + radarDispSize);

        // Render radar as GPU texture -- single draw call, full resolution
        drawList->AddImage(radarTexID, pMin, pMax,
                           ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 255));

        // Range rings overlay
        float centerX = radarX + radarDispSize * 0.5f;
        float centerY = radarY + radarDispSize * 0.5f;
        float maxR = radarDispSize * 0.5f;
        for (int ring = 1; ring <= 4; ring++) {
            float r = maxR * ring / 4.0f;
            drawList->AddCircle(ImVec2(centerX, centerY), r,
                IM_COL32(0, 100, 0, 120), 64, 1.0f);
        }
        // Center cross
        drawList->AddLine(ImVec2(centerX - 5, centerY), ImVec2(centerX + 5, centerY),
            IM_COL32(0, 200, 0, 180));
        drawList->AddLine(ImVec2(centerX, centerY - 5), ImVec2(centerX, centerY + 5),
            IM_COL32(0, 200, 0, 180));

        // Heading line
        drawList->AddLine(ImVec2(centerX, centerY), ImVec2(centerX, radarY),
            IM_COL32(0, 200, 0, 120), 1.0f);
    } else {
        // Radar off or no data
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec2 pMin = ImVec2(radarX, radarY);
        ImVec2 pMax = ImVec2(radarX + radarDispSize, radarY + radarDispSize);
        drawList->AddRectFilled(pMin, pMax, IM_COL32(0, 5, 0, 255));
        float cx = radarX + radarDispSize * 0.5f;
        float cy = radarY + radarDispSize * 0.5f;
        const char* msg = radarOn_ ? "NO RADAR DATA" : "RADAR OFF";
        drawList->AddText(ImVec2(cx - 50, cy - 8), IM_COL32(0, 150, 0, 200), msg);
    }

    // Detect mouse clicks on the radar PPI for ARPA contact tracking
    {
        ImGuiIO& io = ImGui::GetIO();
        float centerX = radarX + radarDispSize * 0.5f;
        float centerY = radarY + radarDispSize * 0.5f;
        float mx = io.MousePos.x;
        float my = io.MousePos.y;
        float dx = mx - centerX;
        float dy = my - centerY;
        float dist = std::sqrt(dx * dx + dy * dy);
        float maxR = radarDispSize * 0.5f;
        bool overPPI = (dist <= maxR);

        // Scale from display pixels to radar texture pixels
        float scale = (float)radarSize / radarDispSize;
        int relX = (int)(dx * scale);
        int relY = (int)(dy * scale);

        SimBridge::setRadarCursorPosition(relX, relY);
        SimBridge::setRadarMouseDown(overPPI && io.MouseDown[0]);

        // On click (mouse just pressed this frame), track nearest contact
        if (overPPI && ImGui::IsMouseClicked(0)) {
            SimBridge::trackTargetFromCursor();
        }
    }

    // Range label
    {
        float rangeNm = SimBridge::getRadarRangeNm();
        char rangeBuf[64];
        snprintf(rangeBuf, sizeof(rangeBuf), "Range: %.1f NM", rangeNm);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddText(ImVec2(radarX + 5, radarY + 5), IM_COL32(0, 200, 0, 220), rangeBuf);
    }

    // Orientation label
    {
        const char* modes[] = { "N UP", "C UP", "H UP" };
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddText(ImVec2(radarX + radarDispSize - 50, radarY + 5),
            IM_COL32(0, 200, 0, 220), modes[orientationMode_]);
    }

    // Control panel on the right
    renderControls(radarAreaW, 0, controlPanelW, radarAreaH * 0.5f);

    // ARPA table below controls
    renderARPATable(radarAreaW, radarAreaH * 0.5f, controlPanelW, radarAreaH * 0.5f);

    // Close button (ESC or X)
    ImGui::SetCursorPos(ImVec2(screenWidth - 35.0f, 5.0f));
    if (ImGui::Button("X", ImVec2(30, 30))) {
        open = false;
    }

    ImGui::End();
    return open;
}

void RadarDisplay::renderControls(float panelX, float panelY, float panelW, float panelH) {
    ImGui::SetCursorPos(ImVec2(panelX + 10, panelY + 10));
    ImGui::BeginChild("RadarControls", ImVec2(panelW - 20, panelH - 20), true);

    ImGui::TextColored(ImVec4(0.0f, 0.8f, 0.0f, 1.0f), "RADAR CONTROLS");
    ImGui::Separator();

    // On/Off
    if (ImGui::Checkbox("Radar On", &radarOn_)) {
        SimBridge::toggleRadarOn();
    }

    ImGui::Spacing();

    // Range
    ImGui::Text("Range");
    ImGui::SameLine();
    if (ImGui::Button("-##range")) SimBridge::decreaseRadarRange();
    ImGui::SameLine();
    float rangeNm = SimBridge::getRadarRangeNm();
    ImGui::Text("%.1f NM", rangeNm);
    ImGui::SameLine();
    if (ImGui::Button("+##range")) SimBridge::increaseRadarRange();

    ImGui::Spacing();

    // Gain (RadarCalculation uses 0-100 scale)
    if (ImGui::SliderFloat("Gain", &gain_, 0.0f, 100.0f, "%.0f")) {
        SimBridge::setRadarGain(gain_);
    }

    // Sea clutter (0-100 scale)
    if (ImGui::SliderFloat("Sea Clutter", &clutter_, 0.0f, 100.0f, "%.0f")) {
        SimBridge::setRadarClutter(clutter_);
    }

    // Rain clutter (0-100 scale)
    if (ImGui::SliderFloat("Rain Clutter", &rain_, 0.0f, 100.0f, "%.0f")) {
        SimBridge::setRadarRain(rain_);
    }

    ImGui::Spacing();
    ImGui::Separator();

    // Orientation mode
    ImGui::Text("Orientation");
    if (ImGui::RadioButton("North Up", &orientationMode_, 0)) SimBridge::setRadarNorthUp();
    ImGui::SameLine();
    if (ImGui::RadioButton("Course Up", &orientationMode_, 1)) SimBridge::setRadarCourseUp();
    ImGui::SameLine();
    if (ImGui::RadioButton("Head Up", &orientationMode_, 2)) SimBridge::setRadarHeadUp();

    ImGui::Spacing();
    ImGui::Separator();

    // ARPA settings
    ImGui::Text("ARPA Vectors");
    bool trueVec = arpaTrue_;
    if (ImGui::RadioButton("Relative", !trueVec)) {
        arpaTrue_ = false;
        SimBridge::setRadarARPARel();
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("True", trueVec)) {
        arpaTrue_ = true;
        SimBridge::setRadarARPATrue();
    }
    if (ImGui::SliderFloat("Vector (min)", &arpaVectorMinutes_, 1.0f, 30.0f, "%.0f")) {
        SimBridge::setRadarARPAVectors(arpaVectorMinutes_);
    }

    ImGui::Spacing();
    ImGui::Separator();

    // ARPA mode
    ImGui::Text("ARPA Mode");
    int arpaMode = SimBridge::getArpaMode();
    if (ImGui::RadioButton("Manual##arpa", arpaMode == 0)) SimBridge::setArpaMode(0);
    ImGui::SameLine();
    if (ImGui::RadioButton("ARPA##arpa", arpaMode == 1)) SimBridge::setArpaMode(1);
    ImGui::SameLine();
    if (ImGui::RadioButton("Auto##arpa", arpaMode == 2)) SimBridge::setArpaMode(2);

    if (arpaMode >= 1) {
        ImGui::TextWrapped("Click on a contact to track it");
    }
    if (ImGui::Button("Clear All Tracks")) {
        SimBridge::clearAllArpaContacts();
    }

    ImGui::EndChild();
}

void RadarDisplay::renderARPATable(float panelX, float panelY, float panelW, float panelH) {
    ImGui::SetCursorPos(ImVec2(panelX + 10, panelY + 10));
    ImGui::BeginChild("ARPATable", ImVec2(panelW - 20, panelH - 20), true);

    ImGui::TextColored(ImVec4(0.0f, 0.8f, 0.0f, 1.0f), "ARPA CONTACTS");
    ImGui::Separator();

    int count = SimBridge::getARPATracksCount();
    if (count == 0) {
        ImGui::TextDisabled("No tracked contacts");
    } else {
        // Table header
        ImGui::Columns(6, "arpa_cols", true);
        ImGui::SetColumnWidth(0, 35);
        ImGui::SetColumnWidth(1, 45);
        ImGui::SetColumnWidth(2, 45);
        ImGui::SetColumnWidth(3, 45);
        ImGui::SetColumnWidth(4, 45);
        ImGui::SetColumnWidth(5, 45);

        ImGui::TextColored(ImVec4(0, 0.7f, 0, 1), "ID"); ImGui::NextColumn();
        ImGui::TextColored(ImVec4(0, 0.7f, 0, 1), "BRG"); ImGui::NextColumn();
        ImGui::TextColored(ImVec4(0, 0.7f, 0, 1), "RNG"); ImGui::NextColumn();
        ImGui::TextColored(ImVec4(0, 0.7f, 0, 1), "SPD"); ImGui::NextColumn();
        ImGui::TextColored(ImVec4(0, 0.7f, 0, 1), "CPA"); ImGui::NextColumn();
        ImGui::TextColored(ImVec4(0, 0.7f, 0, 1), "TCPA"); ImGui::NextColumn();
        ImGui::Separator();

        for (int i = 0; i < count && i < 50; i++) {
            auto c = SimBridge::getARPAContact(i);
            if (c.displayID == 0 && !c.lost) continue; // skip empty

            ImVec4 color = c.lost ? ImVec4(0.5f, 0.5f, 0, 1) :
                           (c.cpa < 0.5f && c.tcpa > 0 && c.tcpa < 15) ?
                           ImVec4(1.0f, 0.2f, 0.2f, 1) : ImVec4(0, 0.8f, 0, 1);

            ImGui::TextColored(color, "%d", c.displayID); ImGui::NextColumn();
            ImGui::TextColored(color, "%.0f", c.bearing); ImGui::NextColumn();
            ImGui::TextColored(color, "%.1f", c.range); ImGui::NextColumn();
            ImGui::TextColored(color, "%.1f", c.speed); ImGui::NextColumn();
            ImGui::TextColored(color, "%.2f", c.cpa); ImGui::NextColumn();
            ImGui::TextColored(color, "%.1f", c.tcpa); ImGui::NextColumn();
        }
        ImGui::Columns(1);
    }

    ImGui::EndChild();
}

}} // namespace bc::gui

#endif // WITH_WICKED_ENGINE
