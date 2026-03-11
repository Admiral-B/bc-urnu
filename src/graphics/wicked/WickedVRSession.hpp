/*   Bridge Command 5.0 Ship Simulator
     Copyright (C) 2026 James Packer

     This program is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License version 2 as
     published by the Free Software Foundation

     OpenXR session for Wicked Engine (D3D12 graphics binding).
     Manages the XR instance, session, swapchains, input, and frame lifecycle.
     Works with WickedVRView for actual rendering.
     Uses pimpl to avoid OpenXR/D3D12 header pollution. */

#ifndef BC_GRAPHICS_WICKED_VRSESSION_HPP
#define BC_GRAPHICS_WICKED_VRSESSION_HPP

#ifdef WITH_WICKED_ENGINE
#if defined(_WIN64)

#include "WickedEngine.h"
#include "../Types.hpp"

#include <memory>

namespace bc { namespace graphics { namespace wicked {

// VR controller input state per hand
struct VRHandState {
    Vec3 gripPosition;
    Quaternion gripOrientation;
    Vec3 aimPosition;
    Quaternion aimOrientation;
    float selectValue = 0;     // Grip squeeze (0-1)
    float triggerValue = 0;    // Trigger (0-1)
    float thumbstickY = 0;    // Thumbstick Y axis (-1 to +1)
    bool menuPressed = false;
    bool poseValid = false;
};

// Manages an OpenXR session bound to WickedEngine's D3D12 device.
// Handles: instance creation, session lifecycle, swapchain management,
// input actions, frame timing, and layer submission.
class WickedVRSession {
public:
    WickedVRSession();
    ~WickedVRSession();

    // Initialize OpenXR with D3D12 binding from WickedEngine's device.
    // Returns true if VR hardware is available and session was created.
    bool init();

    // Per-frame: poll events, begin frame, locate views.
    // Returns true if rendering should proceed this frame.
    bool beginFrame();

    // Per-frame: submit rendered eyes to compositor.
    // Call after WickedVRView::renderEye() for both eyes.
    void endFrame();

    // Copy a WE render target into the OpenXR swapchain for an eye.
    // Call between beginFrame() and endFrame().
    void submitEyeTexture(int eyeIndex, const wi::graphics::Texture* srcTexture);

    // Get per-eye view data for camera setup
    int getViewCount() const;
    int getEyeWidth() const;
    int getEyeHeight() const;

    // Get eye pose data (valid after beginFrame())
    Vec3 getEyePosition(int eyeIndex) const;
    Quaternion getEyeOrientation(int eyeIndex) const;
    void getEyeFov(int eyeIndex, float& left, float& right, float& up, float& down) const;

    // Input
    const VRHandState& getHandState(int handIndex) const;

    // Apply haptic feedback to a hand (0=left, 1=right)
    void hapticPulse(int handIndex, float amplitude = 0.5f, float durationSec = 0.1f);

    bool isRunning() const;
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};

}}} // namespace bc::graphics::wicked

#endif // _WIN64
#endif // WITH_WICKED_ENGINE
#endif // BC_GRAPHICS_WICKED_VRSESSION_HPP
