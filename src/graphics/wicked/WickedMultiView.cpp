/*   Bridge Command 5.0 Ship Simulator
     Copyright (C) 2026 James Packer

     This program is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License version 2 as
     published by the Free Software Foundation */

#ifdef WITH_WICKED_ENGINE

#include "WickedMultiView.hpp"
#include <cmath>
#include <iostream>
#include <algorithm>

namespace bc { namespace graphics { namespace wicked {

static constexpr float DEG_TO_RAD = 3.14159265358979f / 180.0f;
static const wchar_t* EXTRA_VIEW_CLASS = L"BCExtraView";
static bool s_classRegistered = false;

LRESULT CALLBACK WickedMultiView::extraViewWndProc(HWND hwnd, UINT msg,
                                                     WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        case WM_ERASEBKGND:
            return 1;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

HWND WickedMultiView::createViewWindow(HMONITOR monitor, const std::wstring& title) {
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(monitor, &mi)) return nullptr;

    if (!s_classRegistered) {
        WNDCLASSEXW wcex = {};
        wcex.cbSize = sizeof(WNDCLASSEXW);
        wcex.style = CS_HREDRAW | CS_VREDRAW;
        wcex.lpfnWndProc = extraViewWndProc;
        wcex.hInstance = GetModuleHandle(nullptr);
        wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wcex.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wcex.lpszClassName = EXTRA_VIEW_CLASS;
        RegisterClassExW(&wcex);
        s_classRegistered = true;
    }

    int w = mi.rcMonitor.right - mi.rcMonitor.left;
    int h = mi.rcMonitor.bottom - mi.rcMonitor.top;

    HWND hwnd = CreateWindowExW(
        0, EXTRA_VIEW_CLASS, title.c_str(),
        WS_POPUP | WS_VISIBLE,
        mi.rcMonitor.left, mi.rcMonitor.top, w, h,
        nullptr, nullptr, GetModuleHandle(nullptr), nullptr);

    return hwnd;
}

WickedMultiView::~WickedMultiView() {
    shutdown();
}

bool WickedMultiView::init(HWND mainHwnd, wi::scene::Scene* scene,
                            int viewCount, float fovDegrees,
                            const float* yawOffsets) {
    if (viewCount <= 1) return true;
    weScene = scene;
    baseFovDegrees = fovDegrees;

    int extraCount = viewCount - 1;

    HMONITOR mainMon = MonitorFromWindow(mainHwnd, MONITOR_DEFAULTTOPRIMARY);

    struct MonInfo { HMONITOR handle; RECT rc; };
    std::vector<MonInfo> otherMonitors;

    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR hMon, HDC, LPRECT rc, LPARAM param) -> BOOL {
            auto* vec = reinterpret_cast<std::vector<MonInfo>*>(param);
            vec->push_back({hMon, *rc});
            return TRUE;
        }, reinterpret_cast<LPARAM>(&otherMonitors));

    otherMonitors.erase(
        std::remove_if(otherMonitors.begin(), otherMonitors.end(),
            [mainMon](const MonInfo& m) { return m.handle == mainMon; }),
        otherMonitors.end());

    std::sort(otherMonitors.begin(), otherMonitors.end(),
        [](const MonInfo& a, const MonInfo& b) { return a.rc.left < b.rc.left; });

    if ((int)otherMonitors.size() < extraCount) {
        std::cout << "WickedMultiView: Only " << otherMonitors.size()
                  << " extra monitors available, need " << extraCount << std::endl;
        extraCount = (int)otherMonitors.size();
    }

    if (extraCount == 0) {
        std::cout << "WickedMultiView: No extra monitors found." << std::endl;
        return false;
    }

    views.resize(extraCount);
    auto* device = wi::graphics::GetDevice();

    for (int i = 0; i < extraCount; i++) {
        auto& view = views[i];
        view.yawOffset = yawOffsets[i];
        view.fovDegrees = fovDegrees;
        view.swapChain = std::make_unique<wi::graphics::SwapChain>();
        view.renderPath = std::make_unique<wi::RenderPath3D>();
        view.camera = std::make_unique<wi::scene::CameraComponent>();

        if (view.yawOffset < 0)
            view.name = "Port";
        else if (view.yawOffset > 0)
            view.name = "Starboard";
        else
            view.name = "Extra" + std::to_string(i);

        std::wstring title = L"Bridge Command - " +
            std::wstring(view.name.begin(), view.name.end());

        view.hwnd = createViewWindow(otherMonitors[i].handle, title);
        if (!view.hwnd) {
            std::cerr << "WickedMultiView: Failed to create window for '"
                      << view.name << "'" << std::endl;
            continue;
        }

        RECT clientRect;
        GetClientRect(view.hwnd, &clientRect);
        int w = clientRect.right - clientRect.left;
        int h = clientRect.bottom - clientRect.top;

        wi::graphics::SwapChainDesc scDesc;
        scDesc.width = w;
        scDesc.height = h;
        scDesc.buffer_count = 2;
        scDesc.format = wi::graphics::Format::R10G10B10A2_UNORM;
        scDesc.vsync = true;

        bool success = device->CreateSwapChain(
            &scDesc,
            static_cast<wi::platform::window_type>(view.hwnd),
            view.swapChain.get());

        if (!success) {
            std::cerr << "WickedMultiView: SwapChain failed for '"
                      << view.name << "'" << std::endl;
            DestroyWindow(view.hwnd);
            view.hwnd = nullptr;
            continue;
        }

        // Configure render path
        view.renderPath->init(w, h);
        view.renderPath->scene = weScene;
        view.renderPath->camera = view.camera.get();
        view.renderPath->setSceneUpdateEnabled(false);

        // Match main render path quality settings
        view.renderPath->setSSREnabled(true);
        view.renderPath->setFXAAEnabled(true);
        view.renderPath->setBloomEnabled(true);
        view.renderPath->setLensFlareEnabled(true);
        view.renderPath->setAO(wi::RenderPath3D::AO_HBAO);
        view.renderPath->setAORange(2.0f);
        view.renderPath->setAOPower(2.0f);
        view.renderPath->setEyeAdaptionEnabled(true);
        view.renderPath->setEyeAdaptionKey(0.08f);
        view.renderPath->setLightShaftsEnabled(true);
        view.renderPath->setLightShaftsStrength(0.03f);
        view.renderPath->setExposure(1.1f);
        view.renderPath->setSharpenFilterEnabled(true);
        view.renderPath->setSharpenFilterAmount(0.15f);
        view.renderPath->setDitherEnabled(true);

        // Set up camera
        float aspect = (float)w / (float)h;
        float vfov = 2.0f * std::atan(std::tan(fovDegrees * DEG_TO_RAD * 0.5f) / aspect);
        view.camera->zNearP = view.nearPlane;
        view.camera->zFarP = view.farPlane;
        view.camera->fov = vfov;
        view.camera->width = (float)w;
        view.camera->height = (float)h;

        view.active = true;
        std::cout << "WickedMultiView: '" << view.name << "' " << w << "x" << h
                  << " yaw=" << view.yawOffset << "deg" << std::endl;
    }

    return !views.empty();
}

void WickedMultiView::updateCameras(const DirectX::XMVECTOR& shipQuat,
                                      float camX, float camY, float camZ,
                                      float camYawOffset, float camPitch) {
    for (auto& view : views) {
        if (!view.active) continue;

        float totalYawDeg = camYawOffset + view.yawOffset;
        float lookYawRad = totalYawDeg * DEG_TO_RAD;
        float lookPitchRad = camPitch * DEG_TO_RAD;

        DirectX::XMVECTOR localForward = DirectX::XMVectorSet(
            std::sin(lookYawRad) * std::cos(lookPitchRad),
            -std::sin(lookPitchRad),
            std::cos(lookYawRad) * std::cos(lookPitchRad), 0);

        DirectX::XMVECTOR worldForward = DirectX::XMVector3Rotate(localForward, shipQuat);
        float lookX = camX + DirectX::XMVectorGetX(worldForward) * 100.0f;
        float lookY = camY + DirectX::XMVectorGetY(worldForward) * 100.0f;
        float lookZ = camZ + DirectX::XMVectorGetZ(worldForward) * 100.0f;

        DirectX::XMVECTOR localUp = DirectX::XMVectorSet(0, 1, 0, 0);
        DirectX::XMVECTOR worldUp = DirectX::XMVector3Rotate(localUp, shipQuat);

        DirectX::XMVECTOR vEye = DirectX::XMVectorSet(camX, camY, camZ, 1.0f);
        DirectX::XMVECTOR vAt = DirectX::XMVectorSet(lookX, lookY, lookZ, 1.0f);
        DirectX::XMMATRIX viewMat = DirectX::XMMatrixLookAtLH(vEye, vAt, worldUp);
        DirectX::XMMATRIX invView = DirectX::XMMatrixInverse(nullptr, viewMat);
        view.camera->TransformCamera(invView);
    }
}

void WickedMultiView::renderAndPresent() {
    auto* device = wi::graphics::GetDevice();

    for (auto& view : views) {
        if (!view.active) continue;

        view.renderPath->init(view.swapChain->desc.width, view.swapChain->desc.height);
        view.renderPath->PreUpdate();
        view.renderPath->Update(0);
        view.renderPath->PostUpdate();
        view.renderPath->PreRender();
        view.renderPath->Render();
        view.renderPath->PostRender();

        wi::graphics::CommandList cmd = device->BeginCommandList();
        wi::graphics::Viewport viewport;
        viewport.width = (float)view.swapChain->desc.width;
        viewport.height = (float)view.swapChain->desc.height;
        device->BindViewports(1, &viewport, cmd);
        device->RenderPassBegin(view.swapChain.get(), cmd);
        view.renderPath->Compose(cmd);
        device->RenderPassEnd(cmd);
        device->SubmitCommandLists();
    }
}

void WickedMultiView::shutdown() {
    for (auto& view : views) {
        view.active = false;
        if (view.hwnd) {
            DestroyWindow(view.hwnd);
            view.hwnd = nullptr;
        }
    }
    views.clear();
    weScene = nullptr;
}

}}} // namespace bc::graphics::wicked

#endif // WITH_WICKED_ENGINE
