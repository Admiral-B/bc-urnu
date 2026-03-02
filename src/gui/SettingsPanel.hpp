/*   Bridge Command 5.0 Ship Simulator
     Copyright (C) 2024 James Packer

     This program is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License version 2 as
     published by the Free Software Foundation */

#ifndef BC_GUI_SETTINGS_PANEL_HPP
#define BC_GUI_SETTINGS_PANEL_HPP

#ifdef WITH_WICKED_ENGINE

#include "IniFileRW.hpp"
#include <string>

namespace bc { namespace gui {

class SettingsPanel {
public:
    SettingsPanel();
    ~SettingsPanel();

    // Load settings from bc5.ini (checks user dir first, then install dir)
    void load(const std::string& userFolder);

    // Render the settings tabs. Returns true if panel should stay open.
    bool render(int screenWidth, int screenHeight);

    // Save modified settings to user dir
    bool save();

    // True if any setting was changed since last save
    bool isDirty() const { return dirty_; }

    // True if a setting change requires restart to take effect
    bool needsRestart() const { return needsRestart_; }

private:
    bc::ini::IniFile ini_;
    std::string userFolder_;
    std::string userIniPath_;
    bool dirty_ = false;
    bool needsRestart_ = false;
    bool loaded_ = false;

    // Tab renderers
    void renderGraphicsTab();
    void renderSoundTab();
    void renderControlsTab();
    void renderNetworkTab();
    void renderStartupTab();

    // Helper to render a single setting row with description tooltip
    bool renderIntSetting(const char* label, const char* key, int minVal, int maxVal);
    bool renderFloatSetting(const char* label, const char* key, float minVal, float maxVal, const char* fmt = "%.2f");
    bool renderBoolSetting(const char* label, const char* key);
    bool renderComboSetting(const char* label, const char* key, const char* const* items, int itemCount);
    bool renderStringSetting(const char* label, const char* key, int maxLen = 256);
};

}} // namespace bc::gui

#endif // WITH_WICKED_ENGINE
#endif // BC_GUI_SETTINGS_PANEL_HPP
