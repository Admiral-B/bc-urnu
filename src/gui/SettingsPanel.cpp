/*   Bridge Command 5.0 Ship Simulator
     Copyright (C) 2024 James Packer

     This program is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License version 2 as
     published by the Free Software Foundation */

#ifdef WITH_WICKED_ENGINE

#include "SettingsPanel.hpp"
#include "../graphics/wicked/imgui/imgui.h"
#include "../Utilities.hpp"
#include <algorithm>
#include <cstring>

namespace bc { namespace gui {

SettingsPanel::SettingsPanel() {}
SettingsPanel::~SettingsPanel() {}

void SettingsPanel::load(const std::string& userFolder) {
    userFolder_ = userFolder;
    userIniPath_ = userFolder + "bc5.ini";

    // Try user dir first, then install dir
    if (Utilities::pathExists(userIniPath_)) {
        ini_.load(userIniPath_);
    } else if (Utilities::pathExists("bc5.ini")) {
        ini_.load("bc5.ini");
    }
    loaded_ = true;
    dirty_ = false;
    needsRestart_ = false;
}

bool SettingsPanel::save() {
    if (!loaded_) return false;
    bool ok = ini_.save(userIniPath_);
    if (ok) dirty_ = false;
    return ok;
}

bool SettingsPanel::render(int screenWidth, int screenHeight) {
    if (!loaded_) return false;

    bool open = true;
    float panelW = std::min(700.0f, screenWidth * 0.8f);
    float panelH = std::min(550.0f, screenHeight * 0.75f);
    ImGui::SetNextWindowSize(ImVec2(panelW, panelH), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(
        ImVec2(screenWidth * 0.5f, screenHeight * 0.5f),
        ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
    if (!ImGui::Begin("Settings", &open, flags)) {
        ImGui::End();
        return open;
    }

    if (ImGui::BeginTabBar("SettingsTabs")) {
        if (ImGui::BeginTabItem("Graphics")) {
            renderGraphicsTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Sound")) {
            renderSoundTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Controls")) {
            renderControlsTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Network")) {
            renderNetworkTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Startup")) {
            renderStartupTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::Separator();

    if (dirty_) {
        if (ImGui::Button("Save")) {
            save();
        }
        ImGui::SameLine();
        if (needsRestart_) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Some changes require restart");
        }
    } else {
        ImGui::TextDisabled("No changes");
    }

    ImGui::End();
    return open;
}

// ── Tab renderers ───────────────────────────────────────────────────────────

void SettingsPanel::renderGraphicsTab() {
    ImGui::TextDisabled("Display");
    ImGui::Separator();

    const char* gfxModes[] = { "Full Screen", "Windowed", "Borderless" };
    int mode = (int)ini_.getUInt("graphics_mode", 3);
    int modeIdx = (mode == 1) ? 0 : (mode == 2) ? 1 : 2;
    if (ImGui::Combo("Window Mode", &modeIdx, gfxModes, 3)) {
        int newMode = (modeIdx == 0) ? 1 : (modeIdx == 1) ? 2 : 3;
        ini_.setUInt("graphics_mode", newMode);
        dirty_ = true;
        needsRestart_ = true;
    }
    if (ini_.hasKey("graphics_mode")) {
        std::string desc = ini_.getDescription("graphics_mode");
        if (!desc.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", desc.c_str());
    }

    if (renderIntSetting("Monitor", "monitor", 0, 8)) needsRestart_ = true;
    if (renderIntSetting("Width (0=auto)", "graphics_width", 0, 7680)) needsRestart_ = true;
    if (renderIntSetting("Height (0=auto)", "graphics_height", 0, 4320)) needsRestart_ = true;

    ImGui::Spacing();
    ImGui::TextDisabled("Rendering");
    ImGui::Separator();

    renderIntSetting("View Angle", "view_angle", 30, 170);
    renderFloatSetting("Min Distance (m)", "minimum_distance", 0.01f, 10.0f, "%.2f");
    renderFloatSetting("Max Distance (m)", "maximum_distance", 1000.0f, 200000.0f, "%.0f");
    renderIntSetting("Anti-aliasing", "anti_alias", 0, 16);
    renderBoolSetting("Disable Shaders", "disable_shaders");
    renderBoolSetting("VR Mode", "vr_mode");
    renderBoolSetting("Debug Mode", "debug_mode");

    ImGui::Spacing();
    ImGui::TextDisabled("Font");
    ImGui::Separator();

    const char* fonts[] = { "noto-sans", "open-sans", "tinos" };
    std::string currentFont = ini_.getString("font", "noto-sans");
    int fontIdx = 0;
    for (int i = 0; i < 3; i++) {
        if (currentFont == fonts[i]) fontIdx = i;
    }
    if (ImGui::Combo("Font", &fontIdx, fonts, 3)) {
        ini_.setString("font", fonts[fontIdx]);
        dirty_ = true;
        needsRestart_ = true;
    }
    if (renderFloatSetting("Font Scale", "font_scale", 0.5f, 3.0f, "%.1f")) needsRestart_ = true;
}

void SettingsPanel::renderSoundTab() {
    renderFloatSetting("Wave Volume", "wave_volume", 0.0f, 1.0f, "%.2f");
}

void SettingsPanel::renderControlsTab() {
    ImGui::TextDisabled("Joystick Axes");
    ImGui::Separator();

    renderIntSetting("Port Throttle Channel", "port_throttle_channel", 0, 32);
    renderIntSetting("Stbd Throttle Channel", "stbd_throttle_channel", 0, 32);
    renderIntSetting("Rudder Channel", "rudder_channel", 0, 32);
    renderIntSetting("Bow Thruster Channel", "bow_thruster_channel", 0, 32);
    renderIntSetting("Stern Thruster Channel", "stern_thruster_channel", 0, 32);
    renderBoolSetting("Invert Rudder", "invert_rudder");
    renderBoolSetting("Update Changed Axes Only", "update_changed_axes_only");

    ImGui::Spacing();
    ImGui::TextDisabled("Azimuth Controls");
    ImGui::Separator();

    renderIntSetting("Port Thrust Lever Ch", "portThrustLever_channel", 0, 32);
    renderIntSetting("Stbd Thrust Lever Ch", "stbdThrustLever_channel", 0, 32);
    renderIntSetting("Port Schottel Ch", "portSchottel_channel", 0, 32);
    renderIntSetting("Stbd Schottel Ch", "stbdSchottel_channel", 0, 32);
    renderBoolSetting("Invert Port Schottel", "invertPortSchottel");
    renderBoolSetting("Invert Stbd Schottel", "invertStbdSchottel");
}

void SettingsPanel::renderNetworkTab() {
    renderIntSetting("UDP Port", "udp_send_port", 1024, 65535);

    ImGui::Spacing();
    ImGui::TextDisabled("NMEA");
    ImGui::Separator();

    renderStringSetting("NMEA COM Port", "NMEA_ComPort");
    renderStringSetting("NMEA Baudrate", "NMEA_Baudrate");
    renderStringSetting("NMEA UDP Address", "NMEA_UDPAddress");
    renderStringSetting("NMEA UDP Port", "NMEA_UDPPort");
    renderStringSetting("NMEA Listen Port", "NMEA_UDPListenPort");
}

void SettingsPanel::renderStartupTab() {
    renderBoolSetting("Secondary Mode", "secondary_mode");
    renderBoolSetting("Hide Instruments", "hide_instruments");
    renderBoolSetting("Full Radar", "full_radar");
    renderBoolSetting("ARPA On", "arpa_on");

    const char* radarModes[] = { "North Up", "Course Up", "Head Up" };
    int rMode = (int)ini_.getUInt("radar_mode", 0);
    if (rMode < 0 || rMode > 2) rMode = 0;
    if (ImGui::Combo("Radar Mode", &rMode, radarModes, 3)) {
        ini_.setUInt("radar_mode", rMode);
        dirty_ = true;
    }
    std::string desc = ini_.getDescription("radar_mode");
    if (!desc.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", desc.c_str());
}

// ── Setting helpers ─────────────────────────────────────────────────────────

bool SettingsPanel::renderIntSetting(const char* label, const char* key, int minVal, int maxVal) {
    int val = (int)ini_.getUInt(key, 0);
    if (ImGui::SliderInt(label, &val, minVal, maxVal)) {
        ini_.setUInt(key, (uint32_t)val);
        dirty_ = true;
        return true;
    }
    std::string desc = ini_.getDescription(key);
    if (!desc.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", desc.c_str());
    return false;
}

bool SettingsPanel::renderFloatSetting(const char* label, const char* key, float minVal, float maxVal, const char* fmt) {
    float val = ini_.getFloat(key, 0.0f);
    if (ImGui::SliderFloat(label, &val, minVal, maxVal, fmt)) {
        ini_.setFloat(key, val);
        dirty_ = true;
        return true;
    }
    std::string desc = ini_.getDescription(key);
    if (!desc.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", desc.c_str());
    return false;
}

bool SettingsPanel::renderBoolSetting(const char* label, const char* key) {
    bool val = ini_.getUInt(key, 0) != 0;
    if (ImGui::Checkbox(label, &val)) {
        ini_.setUInt(key, val ? 1 : 0);
        dirty_ = true;
        return true;
    }
    std::string desc = ini_.getDescription(key);
    if (!desc.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", desc.c_str());
    return false;
}

bool SettingsPanel::renderComboSetting(const char* label, const char* key, const char* const* items, int itemCount) {
    int idx = (int)ini_.getUInt(key, 0);
    if (idx < 0 || idx >= itemCount) idx = 0;
    if (ImGui::Combo(label, &idx, items, itemCount)) {
        ini_.setUInt(key, (uint32_t)idx);
        dirty_ = true;
        return true;
    }
    std::string desc = ini_.getDescription(key);
    if (!desc.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", desc.c_str());
    return false;
}

bool SettingsPanel::renderStringSetting(const char* label, const char* key, int maxLen) {
    std::string val = ini_.getString(key, "");
    char buf[512];
    int len = std::min((int)val.size(), (int)sizeof(buf) - 1);
    std::memcpy(buf, val.c_str(), len);
    buf[len] = '\0';
    if (ImGui::InputText(label, buf, sizeof(buf))) {
        ini_.setString(key, std::string(buf));
        dirty_ = true;
        return true;
    }
    std::string desc = ini_.getDescription(key);
    if (!desc.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", desc.c_str());
    return false;
}

}} // namespace bc::gui

#endif // WITH_WICKED_ENGINE
