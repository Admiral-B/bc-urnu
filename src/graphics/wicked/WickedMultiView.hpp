/*   Bridge Command 5.0 Ship Simulator
     Copyright (C) 2026 James Packer

     This program is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License version 2 as
     published by the Free Software Foundation

     Multi-viewport rendering for bridge simulator.
     Manages multiple cameras and render targets for the 3-monitor bridge setup.

     Each extra view gets its own RenderPath3D, CameraComponent, and SwapChain.
     The main view (index 0) is rendered by the Application's own render path;
     extra views (1, 2, ...) are rendered after Application::Run() each frame. */

#ifndef BC_GRAPHICS_WICKED_MULTIVIEW_HPP
#define BC_GRAPHICS_WICKED_MULTIVIEW_HPP

#ifdef WITH_WICKED_ENGINE

#include "WickedEngine.h"
#include "../Types.hpp"
#include <vector>
#include <string>
#include <memory>

namespace bc { namespace graphics { namespace wicked {

// Represents a single extra bridge view (one monitor/window).
// View 0 (center) is the main application window -- not managed here.
// Uses unique_ptr for non-copyable WE types so ExtraView can live in a vector.
struct ExtraView {
    std::string name;                    // e.g. "Port", "Starboard"
    HWND hwnd = nullptr;                 // Win32 window handle
    std::unique_ptr<wi::graphics::SwapChain> swapChain;
    std::unique_ptr<wi::RenderPath3D> renderPath;
    std::unique_ptr<wi::scene::CameraComponent> camera;

    // Camera parameters
    float yawOffset = 0;                 // Horizontal angle offset from bow (degrees)
    float fovDegrees = 60.0f;
    float nearPlane = 0.5f;
    float farPlane = 50000.0f;           // 50km for maritime distances

    bool active = false;
};

// Manages extra bridge views for multi-monitor bridge setup.
// The main view (center) is managed by WickedMain's BCRenderPath.
// This class creates and renders additional views (port, starboard, etc).
class WickedMultiView {
public:
    WickedMultiView() = default;
    ~WickedMultiView();

    // Create extra view windows on available monitors.
    // mainHwnd: the main application window (used to determine which monitor it's on)
    // viewCount: total views including main (e.g. 3 = main + port + starboard)
    // fovDegrees: horizontal FOV per view
    // yawOffsets: array of yaw offsets in degrees for each extra view (size = viewCount-1)
    //   e.g. for 3-view: yawOffsets = {-60, +60} (port=-60, starboard=+60)
    bool init(HWND mainHwnd, wi::scene::Scene* scene,
              int viewCount, float fovDegrees,
              const float* yawOffsets);

    // Update cameras for all extra views from the main camera state.
    // Called each frame after the main camera has been positioned.
    // shipQuat: full ship orientation quaternion (heading + pitch + roll)
    // camPos: bridge camera position in world space
    // camYawOffset: current mouse-look yaw offset (degrees)
    void updateCameras(const DirectX::XMVECTOR& shipQuat,
                       float camX, float camY, float camZ,
                       float camYawOffset, float camPitch);

    // Render and present all extra views.
    // Called after application.Run() each frame.
    void renderAndPresent();

    int getExtraViewCount() const { return (int)views.size(); }
    bool hasExtraViews() const { return !views.empty(); }

    void shutdown();

private:
    wi::scene::Scene* weScene = nullptr;
    std::vector<ExtraView> views;
    float baseFovDegrees = 60.0f;

    // Create a borderless window on a specific monitor
    static HWND createViewWindow(HMONITOR monitor, const std::wstring& title);
    // WndProc for extra view windows
    static LRESULT CALLBACK extraViewWndProc(HWND hwnd, UINT msg,
                                              WPARAM wParam, LPARAM lParam);
};

}}} // namespace bc::graphics::wicked

#endif // WITH_WICKED_ENGINE
#endif // BC_GRAPHICS_WICKED_MULTIVIEW_HPP
