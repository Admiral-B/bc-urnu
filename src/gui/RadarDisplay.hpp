/*   Bridge Command 5.0 Ship Simulator
     Copyright (C) 2024 James Packer

     This program is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License version 2 as
     published by the Free Software Foundation */

#ifndef BC_GUI_RADAR_DISPLAY_HPP
#define BC_GUI_RADAR_DISPLAY_HPP

#ifdef WITH_WICKED_ENGINE

#include <cstdint>

// Forward declare ImTextureID to avoid pulling in imgui.h
#ifndef ImTextureID
typedef void* ImTextureID;
#endif

namespace bc { namespace gui {

class RadarDisplay {
public:
    RadarDisplay();
    ~RadarDisplay();

    // Render the full-screen radar overlay. Returns true if still open.
    // radarTexID: GPU texture created from radar pixel data (ImTextureID = Texture*)
    // If radarTexID is nullptr, falls back to "no data" display.
    bool render(int screenWidth, int screenHeight,
                ImTextureID radarTexID, int radarSize);

private:
    // Radar control state
    float gain_ = 50.0f;
    float clutter_ = 0.0f;
    float rain_ = 0.0f;
    int orientationMode_ = 0;  // 0=NorthUp, 1=CourseUp, 2=HeadUp
    bool arpaTrue_ = false;     // true=true vectors, false=relative
    float arpaVectorMinutes_ = 6.0f;
    bool radarOn_ = true;

    void renderControls(float panelX, float panelY, float panelW, float panelH);
    void renderARPATable(float panelX, float panelY, float panelW, float panelH);
};

}} // namespace bc::gui

#endif // WITH_WICKED_ENGINE
#endif // BC_GUI_RADAR_DISPLAY_HPP
