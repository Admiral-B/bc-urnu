/*   Bridge Command 5.0 Ship Simulator
     Copyright (C) 2026 James Packer

     This program is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License version 2 as
     published by the Free Software Foundation */

#ifdef WITH_WICKED_ENGINE

#include "ImGuiOverlay.hpp"
#include "../graphics/wicked/imgui/imgui.h"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace bc { namespace gui {

static constexpr float PI = 3.14159265358979f;
static constexpr float DEG_TO_RAD = PI / 180.0f;
static constexpr float METRES_TO_FEET = 3.28084f;

ImGuiOverlay::ImGuiOverlay() {
    depthHistory_.fill(0.0f);
}
ImGuiOverlay::~ImGuiOverlay() = default;

void ImGuiOverlay::init(int screenWidth, int screenHeight) {
    screenWidth_ = screenWidth;
    screenHeight_ = screenHeight;
    applyPalette();
}

void ImGuiOverlay::setSimulationData(const SimulationHUDData& data) {
    data_ = data;

    // Track depth history for trend indication
    depthHistory_[depthHistoryIndex_] = data.depth;
    depthHistoryIndex_ = (depthHistoryIndex_ + 1) % DEPTH_HISTORY_SIZE;
    if (depthHistoryIndex_ == 0) depthHistoryFull_ = true;
}

void ImGuiOverlay::setControlValues(float portEngine, float stbdEngine, float wheel, float bowThruster) {
    // Only update if user isn't actively dragging a slider
    if (!controlActive_) {
        controlPortEngine_ = portEngine;
        controlStbdEngine_ = stbdEngine;
        controlWheel_ = wheel;
        controlBowThruster_ = bowThruster;
    }
}

void ImGuiOverlay::render() {
    processKeyboardShortcuts();
    controlActive_ = false; // reset each frame, renderControls sets if active

    if (showCompass_) renderCompass();
    if (showSpeed_) renderSpeedDisplay();
    if (showRudder_) renderRudderDisplay();
    if (showDepth_) renderDepthDisplay();
    if (showEngine_) renderEngineDisplay();
    if (showWind_) renderWindDisplay();
    if (showControls_) renderControls();
}

// -- Compass ----------------------------------------------------------

void ImGuiOverlay::renderCompass() {
    ImGui::SetNextWindowSize(ImVec2(400, 70), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(
        ImVec2(screenWidth_ * 0.5f - 200, 10), ImGuiCond_FirstUseEver);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse
        | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoTitleBar;
    if (layoutLocked_)
        flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;

    if (!ImGui::Begin("Heading", nullptr, flags)) {
        ImGui::End();
        return;
    }

    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 winPos = ImGui::GetCursorScreenPos();
    float avail = ImGui::GetContentRegionAvail().x;
    float tapeHeight = 30.0f;
    float tapeTop = winPos.y;
    float tapeBot = tapeTop + tapeHeight;
    float centerX = winPos.x + avail * 0.5f;

    // Pixels per degree -- how many degrees visible across the tape
    float degsVisible = 60.0f;
    float pxPerDeg = avail / degsVisible;

    // Background
    draw->AddRectFilled(
        ImVec2(winPos.x, tapeTop),
        ImVec2(winPos.x + avail, tapeBot),
        IM_COL32(20, 20, 25, 220), 2.0f);

    // Clip to tape area
    draw->PushClipRect(ImVec2(winPos.x, tapeTop), ImVec2(winPos.x + avail, tapeBot + 16));

    // Cardinal/intercardinal labels
    const char* cardinals[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    float cardinalDegs[] = {0, 45, 90, 135, 180, 225, 270, 315};

    // Draw ticks and labels for every degree in range
    float hdg = data_.heading;
    float halfRange = degsVisible * 0.5f + 5; // extra margin

    for (int d = -((int)halfRange + 1); d <= (int)halfRange + 1; d++) {
        float deg = std::fmod(hdg + d + 720.0f, 360.0f);
        int ideg = ((int)std::round(deg)) % 360;
        if (ideg < 0) ideg += 360;

        float xPos = centerX + d * pxPerDeg;
        if (xPos < winPos.x - 20 || xPos > winPos.x + avail + 20) continue;

        if (ideg % 10 == 0) {
            // Major tick every 10 degrees
            float tickLen = (ideg % 30 == 0) ? tapeHeight * 0.6f : tapeHeight * 0.4f;
            draw->AddLine(
                ImVec2(xPos, tapeBot),
                ImVec2(xPos, tapeBot - tickLen),
                IM_COL32(160, 160, 160, 255), 1.0f);

            // Degree number for every 10
            char degStr[8];
            snprintf(degStr, sizeof(degStr), "%03d", ideg);
            ImVec2 textSize = ImGui::CalcTextSize(degStr);
            draw->AddText(
                ImVec2(xPos - textSize.x * 0.5f, tapeTop + 1),
                IM_COL32(150, 150, 150, 220), degStr);
        } else if (ideg % 5 == 0) {
            // Minor tick every 5 degrees
            draw->AddLine(
                ImVec2(xPos, tapeBot),
                ImVec2(xPos, tapeBot - tapeHeight * 0.25f),
                IM_COL32(100, 100, 100, 200), 1.0f);
        }

        // Cardinal labels at exact cardinal positions
        for (int c = 0; c < 8; c++) {
            if (ideg == (int)cardinalDegs[c]) {
                ImU32 col = (c == 0) ? IM_COL32(255, 80, 80, 255) : IM_COL32(220, 200, 100, 255);
                ImVec2 textSize = ImGui::CalcTextSize(cardinals[c]);
                draw->AddText(
                    ImVec2(xPos - textSize.x * 0.5f, tapeBot - tapeHeight + 1),
                    col, cardinals[c]);
            }
        }
    }

    draw->PopClipRect();

    // Center lubber line (fixed marker showing current heading)
    draw->AddTriangleFilled(
        ImVec2(centerX, tapeBot + 2),
        ImVec2(centerX - 5, tapeBot + 10),
        ImVec2(centerX + 5, tapeBot + 10),
        IM_COL32(255, 200, 0, 255));
    draw->AddLine(
        ImVec2(centerX, tapeTop), ImVec2(centerX, tapeBot),
        IM_COL32(255, 200, 0, 180), 2.0f);

    // Numeric heading readout below tape
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + tapeHeight + 12);
    char headingStr[32];
    snprintf(headingStr, sizeof(headingStr), "%05.1f", data_.heading);
    float textW = ImGui::CalcTextSize(headingStr).x;
    ImGui::SetCursorPosX((avail - textW) * 0.5f);
    ImGui::TextColored(ImVec4(1, 0.9f, 0.3f, 1), "%s", headingStr);

    ImGui::End();
}

// -- Speed Display ----------------------------------------------------

void ImGuiOverlay::renderSpeedDisplay() {
    ImGui::SetNextWindowSize(ImVec2(210, 130), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(
        ImVec2(screenWidth_ - 220, 10), ImGuiCond_FirstUseEver);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar;
    if (layoutLocked_) flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;

    if (!ImGui::Begin("Speed", nullptr, flags)) {
        ImGui::End();
        return;
    }

    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1), "SOG");
    ImGui::SameLine(100);
    ImGui::TextColored(ImVec4(1, 1, 0.3f, 1), "%4.1f kn", data_.speedOverGround);

    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1), "STW");
    ImGui::SameLine(100);
    ImGui::TextColored(ImVec4(0.3f, 1, 0.3f, 1), "%4.1f kn", data_.speedThroughWater);

    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1), "COG");
    ImGui::SameLine(100);
    if (data_.speedOverGround >= 0.5f) {
        ImGui::Text("%05.1f", data_.courseOverGround);
    } else {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), " ---");
    }

    ImGui::End();
}

// -- Rudder Angle Display --------------------------------------------

void ImGuiOverlay::renderRudderDisplay() {
    ImGui::SetNextWindowSize(ImVec2(500, 75), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(
        ImVec2(screenWidth_ * 0.5f - 250, screenHeight_ - 270), ImGuiCond_FirstUseEver);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse
        | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoTitleBar;
    if (layoutLocked_)
        flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;

    if (!ImGui::Begin("Rudder", nullptr, flags)) {
        ImGui::End();
        return;
    }

    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 winPos = ImGui::GetCursorScreenPos();
    float avail = ImGui::GetContentRegionAvail().x;
    float tapeHeight = 30.0f;
    float tapeTop = winPos.y;
    float tapeBot = tapeTop + tapeHeight;
    float centerX = winPos.x + avail * 0.5f;

    float maxAngle = 40.0f; // max rudder angle on scale
    float pxPerDeg = avail / (maxAngle * 2.0f);

    // Background
    draw->AddRectFilled(
        ImVec2(winPos.x, tapeTop),
        ImVec2(winPos.x + avail, tapeBot),
        IM_COL32(20, 20, 25, 220), 2.0f);

    // Clip to tape area
    draw->PushClipRect(ImVec2(winPos.x, tapeTop), ImVec2(winPos.x + avail, tapeBot + 16));

    // Port (left, red) and Stbd (right, green) labels
    ImVec2 portSize = ImGui::CalcTextSize("Port");
    draw->AddText(ImVec2(winPos.x + 4, tapeTop + 1), IM_COL32(200, 60, 60, 200), "Port");
    ImVec2 stbdSize = ImGui::CalcTextSize("Stbd");
    draw->AddText(ImVec2(winPos.x + avail - stbdSize.x - 4, tapeTop + 1),
                  IM_COL32(60, 200, 60, 200), "Stbd");

    // Draw ticks: major every 10 deg, minor every 5
    for (int d = -(int)maxAngle; d <= (int)maxAngle; d++) {
        if (d % 5 != 0) continue;
        float xPos = centerX + d * pxPerDeg;

        if (d % 10 == 0) {
            float tickLen = (d == 0) ? tapeHeight * 0.6f : tapeHeight * 0.4f;
            ImU32 col = (d == 0) ? IM_COL32(255, 255, 255, 255) : IM_COL32(160, 160, 160, 255);
            draw->AddLine(ImVec2(xPos, tapeBot), ImVec2(xPos, tapeBot - tickLen), col,
                          (d == 0) ? 2.0f : 1.0f);
            if (d != 0) {
                char degStr[8];
                snprintf(degStr, sizeof(degStr), "%d", std::abs(d));
                ImVec2 ts = ImGui::CalcTextSize(degStr);
                draw->AddText(ImVec2(xPos - ts.x * 0.5f, tapeBot - tapeHeight + 1),
                              IM_COL32(150, 150, 150, 220), degStr);
            }
        } else {
            draw->AddLine(ImVec2(xPos, tapeBot), ImVec2(xPos, tapeBot - tapeHeight * 0.25f),
                          IM_COL32(100, 100, 100, 200), 1.0f);
        }
    }

    // Rudder position indicator (yellow triangle moves with rudder angle)
    float rudderX = centerX + data_.rudderAngle * pxPerDeg;
    // Colour by side: red port, green stbd, yellow amidships
    ImU32 indicatorCol;
    if (data_.rudderAngle < -0.5f)
        indicatorCol = IM_COL32(255, 100, 100, 255); // port red
    else if (data_.rudderAngle > 0.5f)
        indicatorCol = IM_COL32(100, 255, 100, 255); // stbd green
    else
        indicatorCol = IM_COL32(255, 200, 0, 255);   // amidships yellow

    draw->AddTriangleFilled(
        ImVec2(rudderX, tapeBot + 2),
        ImVec2(rudderX - 5, tapeBot + 10),
        ImVec2(rudderX + 5, tapeBot + 10),
        indicatorCol);
    draw->AddLine(ImVec2(rudderX, tapeTop), ImVec2(rudderX, tapeBot),
                  indicatorCol, 2.0f);

    draw->PopClipRect();

    // Numeric readout below tape
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + tapeHeight + 12);
    char rudderStr[32];
    snprintf(rudderStr, sizeof(rudderStr), "%+.1f", data_.rudderAngle);
    float textW = ImGui::CalcTextSize(rudderStr).x;
    ImGui::SetCursorPosX((avail - textW) * 0.5f);

    ImVec4 rudderColor = (data_.rudderAngle < -0.5f)
        ? ImVec4(1, 0.3f, 0.3f, 1)
        : (data_.rudderAngle > 0.5f)
            ? ImVec4(0.3f, 1, 0.3f, 1)
            : ImVec4(1, 1, 1, 1);
    ImGui::TextColored(rudderColor, "%s", rudderStr);

    ImGui::End();
}

// -- Depth Display (7-05) ---------------------------------------------

void ImGuiOverlay::renderDepthDisplay() {
    ImGui::SetNextWindowSize(ImVec2(150, 160), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(
        ImVec2(10, screenHeight_ - 170), ImGuiCond_FirstUseEver);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar;
    if (layoutLocked_) flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;

    if (!ImGui::Begin("Depth", nullptr, flags)) {
        ImGui::End();
        return;
    }

    ImDrawList* draw = ImGui::GetWindowDrawList();
    float avail = ImGui::GetContentRegionAvail().x;

    // Convert depth to display units
    float displayDepth = depthUnitsMetric_ ? data_.depth : data_.depth * METRES_TO_FEET;
    float displayAlarm = depthUnitsMetric_ ? data_.depthAlarm : data_.depthAlarm * METRES_TO_FEET;
    const char* unitStr = depthUnitsMetric_ ? "m" : "ft";

    // Alarm state
    bool alarm = data_.depth > 0 && data_.depth < data_.depthAlarm;
    bool alarmFlash = alarm && (std::fmod(data_.simulationTime * 2.0f, 1.0f) > 0.5f);

    // Flashing background when alarm active
    if (alarmFlash) {
        ImVec2 wp = ImGui::GetWindowPos();
        ImVec2 ws = ImGui::GetWindowSize();
        draw->AddRectFilled(wp, ImVec2(wp.x + ws.x, wp.y + ws.y),
            IM_COL32(120, 20, 20, 60), 4.0f);
    }

    // Depth value - large text
    ImVec4 depthColor;
    if (alarm) {
        depthColor = ImVec4(1, 0.2f, 0.2f, 1); // Red when below alarm
    } else if (data_.depth > 0 && data_.depth < data_.depthAlarm * 2.0f) {
        depthColor = ImVec4(1, 0.8f, 0.2f, 1); // Yellow when approaching alarm
    } else {
        depthColor = ImVec4(0.3f, 0.8f, 1, 1);  // Blue for safe depth
    }

    char depthStr[32];
    snprintf(depthStr, sizeof(depthStr), "%.1f %s", displayDepth, unitStr);
    ImVec2 textSize = ImGui::CalcTextSize(depthStr);
    ImGui::SetCursorPosX((avail - textSize.x) * 0.5f);
    ImGui::TextColored(depthColor, "%s", depthStr);

    // Depth trend arrow
    float depthTrend = 0.0f;
    if (depthHistoryFull_ || depthHistoryIndex_ > 5) {
        int oldest = depthHistoryFull_
            ? (depthHistoryIndex_ + 1) % DEPTH_HISTORY_SIZE
            : 0;
        depthTrend = data_.depth - depthHistory_[oldest];
    }

    ImGui::SameLine();
    if (depthTrend > 0.1f) {
        ImGui::TextColored(ImVec4(0.3f, 1, 0.3f, 1), "^"); // Deepening
    } else if (depthTrend < -0.1f) {
        ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "v"); // Shoaling
    } else {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "-"); // Steady
    }

    // Vertical depth gauge bar
    ImGui::Spacing();
    ImVec2 barPos = ImGui::GetCursorScreenPos();
    float barWidth = avail - 30;
    float barHeight = 60;

    // Determine scale range: show 0 to max(depth*2, alarm*3, 20)
    float maxScale = std::fmax(displayDepth * 2.0f, std::fmax(displayAlarm * 3.0f, depthUnitsMetric_ ? 20.0f : 60.0f));
    if (maxScale < 1.0f) maxScale = 1.0f;

    // Bar background
    draw->AddRectFilled(
        ImVec2(barPos.x + 15, barPos.y),
        ImVec2(barPos.x + 15 + barWidth, barPos.y + barHeight),
        IM_COL32(20, 30, 50, 200), 2.0f);

    // Depth fill (from top down)
    float depthFrac = std::fmin(displayDepth / maxScale, 1.0f);
    if (depthFrac > 0) {
        ImU32 fillCol = alarm ? IM_COL32(180, 40, 40, 180) : IM_COL32(40, 100, 180, 180);
        draw->AddRectFilled(
            ImVec2(barPos.x + 15, barPos.y),
            ImVec2(barPos.x + 15 + barWidth, barPos.y + barHeight * depthFrac),
            fillCol, 2.0f);
    }

    // Alarm threshold marker (horizontal dashed line)
    float alarmFrac = std::fmin(displayAlarm / maxScale, 1.0f);
    float alarmY = barPos.y + barHeight * alarmFrac;
    for (float x = barPos.x + 15; x < barPos.x + 15 + barWidth; x += 8) {
        draw->AddLine(
            ImVec2(x, alarmY),
            ImVec2(std::fmin(x + 4, barPos.x + 15 + barWidth), alarmY),
            IM_COL32(255, 80, 80, 200), 1.5f);
    }

    // Depth marker (current depth line)
    float depthY = barPos.y + barHeight * depthFrac;
    draw->AddLine(
        ImVec2(barPos.x + 12, depthY),
        ImVec2(barPos.x + 18 + barWidth, depthY),
        IM_COL32(255, 255, 0, 255), 2.0f);

    // Scale labels
    char topLabel[16], botLabel[16];
    snprintf(topLabel, sizeof(topLabel), "0");
    snprintf(botLabel, sizeof(botLabel), "%.0f", maxScale);
    draw->AddText(ImVec2(barPos.x + 15, barPos.y - 14), IM_COL32(150, 150, 150, 200), topLabel);
    draw->AddText(ImVec2(barPos.x + 15, barPos.y + barHeight + 2), IM_COL32(150, 150, 150, 200), botLabel);

    // Advance cursor past the bar
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + barHeight + 18);

    // Alarm threshold label
    char alarmStr[32];
    snprintf(alarmStr, sizeof(alarmStr), "Alarm: %.1f %s", displayAlarm, unitStr);
    ImVec4 alarmLabelColor = alarm ? ImVec4(1, 0.3f, 0.3f, 1) : ImVec4(0.5f, 0.5f, 0.5f, 1);
    ImGui::TextColored(alarmLabelColor, "%s", alarmStr);

    // Unit toggle (click to switch)
    ImGui::SameLine(avail - 20);
    if (ImGui::SmallButton(depthUnitsMetric_ ? "m" : "ft")) {
        depthUnitsMetric_ = !depthUnitsMetric_;
    }

    ImGui::End();
}

// -- Engine Display ---------------------------------------------------

void ImGuiOverlay::renderEngineDisplay() {
    ImGui::SetNextWindowSize(ImVec2(210, 110), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(
        ImVec2(screenWidth_ - 220, 150), ImGuiCond_FirstUseEver);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar;
    if (layoutLocked_) flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;

    if (!ImGui::Begin("Engine", nullptr, flags)) {
        ImGui::End();
        return;
    }

    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1), "RPM");
    ImGui::SameLine(100);
    ImGui::TextColored(ImVec4(0.3f, 1, 0.3f, 1), "%4.0f", data_.engineRPM);

    // RPM bar
    float rpmFrac = std::abs(data_.engineRPM) / 200.0f; // assume 200 RPM max
    if (rpmFrac > 1.0f) rpmFrac = 1.0f;
    ImGui::ProgressBar(rpmFrac, ImVec2(-1, 12), "");

    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1), "Thrust");
    ImGui::SameLine(100);
    ImGui::Text("%+.0f%%", data_.thrustLever * 100);

    ImGui::End();
}

// -- Wind Display -----------------------------------------------------

void ImGuiOverlay::renderWindDisplay() {
    ImGui::SetNextWindowSize(ImVec2(130, 70), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(
        ImVec2(10, 10), ImGuiCond_FirstUseEver);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar;
    if (layoutLocked_) flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;

    if (!ImGui::Begin("Wind", nullptr, flags)) {
        ImGui::End();
        return;
    }

    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1), "Speed");
    ImGui::SameLine(70);
    ImGui::Text("%.0f kn", data_.windSpeed);

    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1), "From");
    ImGui::SameLine(70);
    ImGui::Text("%03.0f", data_.windDirection);

    ImGui::End();
}

// -- Ship Controls (interactive) --------------------------------------

void ImGuiOverlay::renderControls() {
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar;
    if (layoutLocked_) flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;

    // Helper lambda for engine vertical slider
    auto renderEngineSlider = [&](const char* label, const char* sliderId, float& value) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        float avail = ImGui::GetContentRegionAvail().x;

        int pct = (int)std::round(value * 100.0f);
        ImVec4 col = (pct > 0) ? ImVec4(0.3f, 1, 0.3f, 1) :
                     (pct < 0) ? ImVec4(1, 0.3f, 0.3f, 1) :
                                 ImVec4(0.7f, 0.7f, 0.7f, 1);
        // Label
        float labelW = ImGui::CalcTextSize(label).x;
        ImGui::SetCursorPosX((avail - labelW) * 0.5f);
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "%s", label);

        // Percentage
        char str[16];
        snprintf(str, sizeof(str), "%+d%%", pct);
        float textW = ImGui::CalcTextSize(str).x;
        ImGui::SetCursorPosX((avail - textW) * 0.5f);
        ImGui::TextColored(col, "%s", str);

        float sliderHeight = ImGui::GetContentRegionAvail().y - 20;
        if (sliderHeight < 50) sliderHeight = 50;
        ImGui::SetCursorPosX((avail - 36) * 0.5f);
        if (ImGui::VSliderFloat(sliderId, ImVec2(36, sliderHeight), &value, -1.0f, 1.0f, "")) {
            controlActive_ = true;
        }
        if (ImGui::IsItemActive()) controlActive_ = true;

        // Tick marks
        ImVec2 sliderMin = ImGui::GetItemRectMin();
        ImVec2 sliderMax = ImGui::GetItemRectMax();
        float sh = sliderMax.y - sliderMin.y;
        float ticks[] = {1.0f, 0.5f, 0.0f, -0.5f, -1.0f};
        for (int t = 0; t < 5; t++) {
            float frac = (ticks[t] + 1.0f) / 2.0f;
            float y = sliderMax.y - frac * sh;
            draw->AddLine(ImVec2(sliderMax.x + 1, y), ImVec2(sliderMax.x + 5, y),
                          IM_COL32(120, 120, 120, 200), 1.0f);
        }
    };

    // Port + Starboard engine sliders side by side
    ImGui::SetNextWindowSize(ImVec2(190, 340), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(
        ImVec2(10, screenHeight_ * 0.5f - 170), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Engines##ctrl", nullptr, flags)) {
        float avail = ImGui::GetContentRegionAvail().x;
        float colW = avail * 0.5f - 4;

        // Port engine (left column)
        ImGui::BeginChild("##portCol", ImVec2(colW, 0), false);
        renderEngineSlider("Port", "##portEng", controlPortEngine_);
        ImGui::EndChild();

        ImGui::SameLine(0, 8);

        // Starboard engine (right column)
        ImGui::BeginChild("##stbdCol", ImVec2(colW, 0), false);
        renderEngineSlider("Stbd", "##stbdEng", controlStbdEngine_);
        ImGui::EndChild();
    }
    ImGui::End();

    // Bow thruster - horizontal slider
    ImGui::SetNextWindowSize(ImVec2(380, 80), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(
        ImVec2(10, screenHeight_ * 0.5f + 180), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Bow Thruster##ctrl", nullptr, flags)) {
        float avail = ImGui::GetContentRegionAvail().x;
        ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 0.7f), "P");
        ImGui::SameLine(avail - ImGui::CalcTextSize("S").x);
        ImGui::TextColored(ImVec4(0.3f, 1, 0.3f, 0.7f), "S");
        ImGui::SetNextItemWidth(avail);
        if (ImGui::SliderFloat("##bowThr", &controlBowThruster_, -1.0f, 1.0f, "%+.0f%%")) {
            controlActive_ = true;
        }
        if (ImGui::IsItemActive()) controlActive_ = true;
    }
    ImGui::End();

    // Steering wheel - horizontal slider at bottom center
    ImGui::SetNextWindowSize(ImVec2(500, 85), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(
        ImVec2(screenWidth_ * 0.5f - 250, screenHeight_ - 100), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Wheel##ctrl", nullptr, flags)) {
        float avail = ImGui::GetContentRegionAvail().x;

        ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 0.7f), "Port");
        ImGui::SameLine(avail - ImGui::CalcTextSize("Stbd").x);
        ImGui::TextColored(ImVec4(0.3f, 1, 0.3f, 0.7f), "Stbd");

        ImGui::SetNextItemWidth(avail);
        if (ImGui::SliderFloat("##wheel", &controlWheel_, -30.0f, 30.0f, "%.0f")) {
            controlActive_ = true;
        }
        if (ImGui::IsItemActive()) controlActive_ = true;
    }
    ImGui::End();
}

// -- Visibility toggles -----------------------------------------------

void ImGuiOverlay::showCompass(bool show) { showCompass_ = show; }
void ImGuiOverlay::showSpeedDisplay(bool show) { showSpeed_ = show; }
void ImGuiOverlay::showRudderDisplay(bool show) { showRudder_ = show; }
void ImGuiOverlay::showDepthDisplay(bool show) { showDepth_ = show; }
void ImGuiOverlay::showEngineDisplay(bool show) { showEngine_ = show; }
void ImGuiOverlay::showWindDisplay(bool show) { showWind_ = show; }

void ImGuiOverlay::showControls(bool show) { showControls_ = show; }

void ImGuiOverlay::showAll(bool show) {
    showCompass_ = showSpeed_ = showRudder_ = showDepth_ = showEngine_ = showWind_ = showControls_ = show;
}

void ImGuiOverlay::setLayoutLocked(bool locked) { layoutLocked_ = locked; }

void ImGuiOverlay::setDepthUnitMetric(bool metric) { depthUnitsMetric_ = metric; }

// -- Keyboard shortcuts (7-06) ----------------------------------------

void ImGuiOverlay::processKeyboardShortcuts() {
    // F5 = Bright Day, F6 = Day, F7 = Dusk, F8 = Night
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) setPalette(0);
    if (ImGui::IsKeyPressed(ImGuiKey_F6, false)) setPalette(1);
    if (ImGui::IsKeyPressed(ImGuiKey_F7, false)) setPalette(2);
    if (ImGui::IsKeyPressed(ImGuiKey_F8, false)) setPalette(3);

    // F9 = Toggle layout lock
    if (ImGui::IsKeyPressed(ImGuiKey_F9, false)) {
        layoutLocked_ = !layoutLocked_;
    }

    // F10 = Toggle depth units (m/ft)
    if (ImGui::IsKeyPressed(ImGuiKey_F10, false)) {
        depthUnitsMetric_ = !depthUnitsMetric_;
    }
}

// -- Palettes (7-06) --------------------------------------------------

void ImGuiOverlay::setPalette(int index) {
    if (index < 0 || index > 3) return;
    currentPalette_ = index;
    applyPalette();
}

void ImGuiOverlay::applyPalette() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 2.0f;
    style.WindowBorderSize = 1.0f;

    ImVec4* colors = style.Colors;

    switch (currentPalette_) {
    case 0: // Bright Day - white backgrounds, high contrast
        colors[ImGuiCol_WindowBg]       = ImVec4(0.95f, 0.95f, 0.95f, 0.9f);
        colors[ImGuiCol_TitleBg]        = ImVec4(0.82f, 0.82f, 0.82f, 1.0f);
        colors[ImGuiCol_TitleBgActive]  = ImVec4(0.72f, 0.72f, 0.72f, 1.0f);
        colors[ImGuiCol_Text]           = ImVec4(0.08f, 0.08f, 0.08f, 1.0f);
        colors[ImGuiCol_Border]         = ImVec4(0.5f, 0.5f, 0.5f, 0.8f);
        colors[ImGuiCol_FrameBg]        = ImVec4(0.85f, 0.85f, 0.85f, 0.6f);
        colors[ImGuiCol_ScrollbarBg]    = ImVec4(0.9f, 0.9f, 0.9f, 0.5f);
        colors[ImGuiCol_ScrollbarGrab]  = ImVec4(0.6f, 0.6f, 0.6f, 0.8f);
        colors[ImGuiCol_Button]         = ImVec4(0.8f, 0.8f, 0.8f, 0.7f);
        colors[ImGuiCol_ButtonHovered]  = ImVec4(0.7f, 0.7f, 0.7f, 0.8f);
        colors[ImGuiCol_ButtonActive]   = ImVec4(0.6f, 0.6f, 0.6f, 0.9f);
        colors[ImGuiCol_PlotHistogram]  = ImVec4(0.2f, 0.5f, 0.2f, 0.8f);
        break;

    case 1: // Day - dark background, standard brightness
        colors[ImGuiCol_WindowBg]       = ImVec4(0.2f, 0.2f, 0.22f, 0.92f);
        colors[ImGuiCol_TitleBg]        = ImVec4(0.15f, 0.15f, 0.17f, 1.0f);
        colors[ImGuiCol_TitleBgActive]  = ImVec4(0.2f, 0.2f, 0.25f, 1.0f);
        colors[ImGuiCol_Text]           = ImVec4(0.9f, 0.9f, 0.9f, 1.0f);
        colors[ImGuiCol_Border]         = ImVec4(0.4f, 0.4f, 0.4f, 0.6f);
        colors[ImGuiCol_FrameBg]        = ImVec4(0.12f, 0.12f, 0.14f, 0.5f);
        colors[ImGuiCol_ScrollbarBg]    = ImVec4(0.15f, 0.15f, 0.17f, 0.5f);
        colors[ImGuiCol_ScrollbarGrab]  = ImVec4(0.4f, 0.4f, 0.4f, 0.6f);
        colors[ImGuiCol_Button]         = ImVec4(0.25f, 0.25f, 0.28f, 0.7f);
        colors[ImGuiCol_ButtonHovered]  = ImVec4(0.35f, 0.35f, 0.38f, 0.8f);
        colors[ImGuiCol_ButtonActive]   = ImVec4(0.4f, 0.4f, 0.45f, 0.9f);
        colors[ImGuiCol_PlotHistogram]  = ImVec4(0.3f, 0.8f, 0.3f, 0.8f);
        break;

    case 2: // Dusk - reduced brightness, warm tones
        colors[ImGuiCol_WindowBg]       = ImVec4(0.12f, 0.11f, 0.10f, 0.95f);
        colors[ImGuiCol_TitleBg]        = ImVec4(0.08f, 0.07f, 0.06f, 1.0f);
        colors[ImGuiCol_TitleBgActive]  = ImVec4(0.14f, 0.12f, 0.10f, 1.0f);
        colors[ImGuiCol_Text]           = ImVec4(0.65f, 0.6f, 0.5f, 1.0f);
        colors[ImGuiCol_Border]         = ImVec4(0.3f, 0.25f, 0.2f, 0.5f);
        colors[ImGuiCol_FrameBg]        = ImVec4(0.08f, 0.07f, 0.06f, 0.5f);
        colors[ImGuiCol_ScrollbarBg]    = ImVec4(0.1f, 0.09f, 0.08f, 0.5f);
        colors[ImGuiCol_ScrollbarGrab]  = ImVec4(0.3f, 0.25f, 0.2f, 0.5f);
        colors[ImGuiCol_Button]         = ImVec4(0.15f, 0.13f, 0.11f, 0.7f);
        colors[ImGuiCol_ButtonHovered]  = ImVec4(0.22f, 0.18f, 0.15f, 0.8f);
        colors[ImGuiCol_ButtonActive]   = ImVec4(0.28f, 0.22f, 0.18f, 0.9f);
        colors[ImGuiCol_PlotHistogram]  = ImVec4(0.5f, 0.4f, 0.2f, 0.8f);
        break;

    case 3: // Night - black background, red/amber only (preserves night vision)
        colors[ImGuiCol_WindowBg]       = ImVec4(0.02f, 0.01f, 0.01f, 0.98f);
        colors[ImGuiCol_TitleBg]        = ImVec4(0.04f, 0.01f, 0.01f, 1.0f);
        colors[ImGuiCol_TitleBgActive]  = ImVec4(0.08f, 0.02f, 0.02f, 1.0f);
        colors[ImGuiCol_Text]           = ImVec4(0.5f, 0.12f, 0.08f, 1.0f);
        colors[ImGuiCol_Border]         = ImVec4(0.15f, 0.04f, 0.04f, 0.6f);
        colors[ImGuiCol_FrameBg]        = ImVec4(0.04f, 0.01f, 0.01f, 0.5f);
        colors[ImGuiCol_ScrollbarBg]    = ImVec4(0.03f, 0.01f, 0.01f, 0.5f);
        colors[ImGuiCol_ScrollbarGrab]  = ImVec4(0.15f, 0.04f, 0.04f, 0.5f);
        colors[ImGuiCol_Button]         = ImVec4(0.08f, 0.02f, 0.02f, 0.7f);
        colors[ImGuiCol_ButtonHovered]  = ImVec4(0.12f, 0.03f, 0.03f, 0.8f);
        colors[ImGuiCol_ButtonActive]   = ImVec4(0.18f, 0.05f, 0.05f, 0.9f);
        colors[ImGuiCol_PlotHistogram]  = ImVec4(0.4f, 0.1f, 0.05f, 0.8f);
        break;
    }
}

// -- Layout save/load (7-07) ------------------------------------------

void ImGuiOverlay::setIniFilePath(const std::string& path) {
    iniFilePath_ = path;
    // ImGui uses a static const char* for IniFilename, so we store the string
    // and point ImGui at our persistent storage
    ImGui::GetIO().IniFilename = iniFilePath_.empty() ? nullptr : iniFilePath_.c_str();
}

void ImGuiOverlay::saveLayout(const std::string& path) {
    std::ofstream file(path);
    if (!file.is_open()) return;

    file << "[ImGuiOverlay]\n";
    file << "Palette=" << currentPalette_ << "\n";
    file << "LayoutLocked=" << (layoutLocked_ ? 1 : 0) << "\n";
    file << "DepthUnitsMetric=" << (depthUnitsMetric_ ? 1 : 0) << "\n";
    file << "ShowCompass=" << (showCompass_ ? 1 : 0) << "\n";
    file << "ShowSpeed=" << (showSpeed_ ? 1 : 0) << "\n";
    file << "ShowRudder=" << (showRudder_ ? 1 : 0) << "\n";
    file << "ShowDepth=" << (showDepth_ ? 1 : 0) << "\n";
    file << "ShowEngine=" << (showEngine_ ? 1 : 0) << "\n";
    file << "ShowWind=" << (showWind_ ? 1 : 0) << "\n";

    // Also trigger ImGui's own window position save
    if (!iniFilePath_.empty()) {
        ImGui::SaveIniSettingsToDisk(iniFilePath_.c_str());
    }
}

void ImGuiOverlay::loadLayout(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return;

    std::string line;
    while (std::getline(file, line)) {
        // Skip section headers and empty lines
        if (line.empty() || line[0] == '[') continue;

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        if (key == "Palette") { setPalette(std::stoi(val)); }
        else if (key == "LayoutLocked") { layoutLocked_ = (val == "1"); }
        else if (key == "DepthUnitsMetric") { depthUnitsMetric_ = (val == "1"); }
        else if (key == "ShowCompass") { showCompass_ = (val == "1"); }
        else if (key == "ShowSpeed") { showSpeed_ = (val == "1"); }
        else if (key == "ShowRudder") { showRudder_ = (val == "1"); }
        else if (key == "ShowDepth") { showDepth_ = (val == "1"); }
        else if (key == "ShowEngine") { showEngine_ = (val == "1"); }
        else if (key == "ShowWind") { showWind_ = (val == "1"); }
    }
}

bool ImGuiOverlay::wantsKeyboard() const {
    return ImGui::GetIO().WantCaptureKeyboard;
}

bool ImGuiOverlay::wantsMouse() const {
    return ImGui::GetIO().WantCaptureMouse;
}

}} // namespace bc::gui

#endif // WITH_WICKED_ENGINE
