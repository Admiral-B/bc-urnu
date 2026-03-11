/*   Bridge Command 5.0 Ship Simulator
     Copyright (C) 2026 James Packer

     This program is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License version 2 as
     published by the Free Software Foundation */

#ifdef WITH_WICKED_ENGINE
#if defined(_WIN64)

#include "WickedVRSession.hpp"

// WE DX12 device header -- must come BEFORE system d3d12.h to use WE's version
#include "wiGraphicsDevice_DX12.h"

// OpenXR with D3D12 binding (WE's d3d12.h provides all D3D12 types)
#define XR_USE_GRAPHICS_API_D3D12
#define XR_USE_PLATFORM_WIN32
#include "../../libs/OpenXR/OpenXR-SDK-main/include/openxr/openxr.h"
#include "../../libs/OpenXR/OpenXR-SDK-main/include/openxr/openxr_platform.h"

#include <iostream>
#include <cstring>
#include <vector>
#include <string>

namespace bc { namespace graphics { namespace wicked {

static constexpr int HAND_LEFT = 0;
static constexpr int HAND_RIGHT = 1;

// Helper: get the DX12 device implementation from WE
static wi::graphics::GraphicsDevice_DX12* getDX12Device() {
    return dynamic_cast<wi::graphics::GraphicsDevice_DX12*>(wi::graphics::GetDevice());
}

// Helper: get ID3D12Resource from WE Texture
static ID3D12Resource* getD3D12Resource(const wi::graphics::Texture* tex) {
    auto* dx12 = getDX12Device();
    if (!dx12 || !tex) return nullptr;
    return dx12->GetTextureInternalResource(tex);
}

// --- Pimpl implementation ---
struct WickedVRSession::Impl {
    // OpenXR handles
    XrInstance instance = XR_NULL_HANDLE;
    XrSession session = XR_NULL_HANDLE;
    XrSpace playSpace = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;

    // Views
    uint32_t viewCount = 0;
    std::vector<XrViewConfigurationView> viewConfigViews;
    std::vector<XrView> views;
    int eyeWidth = 0;
    int eyeHeight = 0;

    // Swapchains (one per eye)
    std::vector<XrSwapchain> swapchains;
    std::vector<std::vector<XrSwapchainImageD3D12KHR>> swapchainImages;
    std::vector<uint32_t> swapchainLengths;

    // Frame state
    XrFrameState frameState = {};
    XrCompositionLayerProjectionView projectionViews[2] = {};
    bool frameActive = false;

    // Input
    XrActionSet actionSet = XR_NULL_HANDLE;
    XrAction gripPoseAction = XR_NULL_HANDLE;
    XrAction aimPoseAction = XR_NULL_HANDLE;
    XrAction selectAction = XR_NULL_HANDLE;
    XrAction triggerAction = XR_NULL_HANDLE;
    XrAction menuAction = XR_NULL_HANDLE;
    XrAction thumbstickYAction = XR_NULL_HANDLE;
    XrAction hapticAction = XR_NULL_HANDLE;
    XrSpace gripPoseSpaces[2] = {};
    XrSpace aimPoseSpaces[2] = {};
    VRHandState handStates[2];

    // Session state
    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    bool sessionRunning = false;
    bool shouldRender = false;

    // D3D12 copy infrastructure
    ID3D12CommandAllocator* copyAllocator = nullptr;
    ID3D12GraphicsCommandList* copyCmdList = nullptr;
    ID3D12CommandQueue* graphicsQueue = nullptr;
    ID3D12Fence* copyFence = nullptr;
    UINT64 copyFenceValue = 0;
    HANDLE copyFenceEvent = nullptr;

    static bool xrCheck(XrInstance inst, XrResult result, const char* msg) {
        if (XR_SUCCEEDED(result)) return true;
        if (inst != XR_NULL_HANDLE) {
            char buf[XR_MAX_RESULT_STRING_SIZE];
            xrResultToString(inst, result, buf);
            std::cerr << "OpenXR error: " << msg << " (" << buf << ")" << std::endl;
        } else {
            std::cerr << "OpenXR error: " << msg << " (result=" << result << ")" << std::endl;
        }
        return false;
    }

    bool createInstance();
    bool createSession();
    bool createSwapchains();
    bool createActions();
    void pollEvents();
    void syncActions();
    void shutdown();
};

// --- WickedVRSession forwarding ---

WickedVRSession::WickedVRSession() : pImpl(std::make_unique<Impl>()) {}
WickedVRSession::~WickedVRSession() { if (pImpl) pImpl->shutdown(); }

bool WickedVRSession::init() {
    if (!pImpl->createInstance()) return false;
    if (!pImpl->createSession()) return false;
    if (!pImpl->createSwapchains()) return false;
    if (!pImpl->createActions()) return false;
    std::cout << "WickedVRSession: OpenXR D3D12 session ready ("
              << pImpl->eyeWidth << "x" << pImpl->eyeHeight << " per eye, "
              << pImpl->viewCount << " views)" << std::endl;
    return true;
}

bool WickedVRSession::beginFrame() {
    auto& d = *pImpl;
    d.pollEvents();
    if (!d.sessionRunning) return false;

    d.frameState = { XR_TYPE_FRAME_STATE };
    XrFrameWaitInfo waitInfo = { XR_TYPE_FRAME_WAIT_INFO };
    XrResult result = xrWaitFrame(d.session, &waitInfo, &d.frameState);
    if (!Impl::xrCheck(d.instance, result, "xrWaitFrame")) return false;

    XrFrameBeginInfo beginInfo = { XR_TYPE_FRAME_BEGIN_INFO };
    result = xrBeginFrame(d.session, &beginInfo);
    if (!Impl::xrCheck(d.instance, result, "xrBeginFrame")) return false;

    d.frameActive = true;
    d.shouldRender = d.frameState.shouldRender;

    if (!d.shouldRender) return false;

    // Locate views (eye positions + FOV)
    XrViewState viewState = { XR_TYPE_VIEW_STATE };
    XrViewLocateInfo locateInfo = { XR_TYPE_VIEW_LOCATE_INFO };
    locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locateInfo.displayTime = d.frameState.predictedDisplayTime;
    locateInfo.space = d.playSpace;
    uint32_t viewCountOut = 0;
    xrLocateViews(d.session, &locateInfo, &viewState, d.viewCount, &viewCountOut, d.views.data());

    d.syncActions();
    return true;
}

void WickedVRSession::endFrame() {
    auto& d = *pImpl;
    if (!d.frameActive) return;

    if (d.shouldRender) {
        XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
        layer.space = d.playSpace;
        layer.viewCount = d.viewCount;
        layer.views = d.projectionViews;
        const XrCompositionLayerBaseHeader* layers[] = { (XrCompositionLayerBaseHeader*)&layer };
        XrFrameEndInfo endInfo = { XR_TYPE_FRAME_END_INFO };
        endInfo.displayTime = d.frameState.predictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endInfo.layerCount = 1;
        endInfo.layers = layers;
        xrEndFrame(d.session, &endInfo);
    } else {
        XrFrameEndInfo endInfo = { XR_TYPE_FRAME_END_INFO };
        endInfo.displayTime = d.frameState.predictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endInfo.layerCount = 0;
        endInfo.layers = nullptr;
        xrEndFrame(d.session, &endInfo);
    }
    d.frameActive = false;
}

void WickedVRSession::submitEyeTexture(int eyeIndex, const wi::graphics::Texture* srcTexture) {
    auto& d = *pImpl;
    if (eyeIndex < 0 || eyeIndex >= (int)d.viewCount) return;
    if (!srcTexture) return;

    // Acquire swapchain image
    XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    uint32_t imgIndex = 0;
    xrAcquireSwapchainImage(d.swapchains[eyeIndex], &acquireInfo, &imgIndex);

    XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    waitInfo.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(d.swapchains[eyeIndex], &waitInfo);

    // Copy WE render target -> OpenXR swapchain D3D12 texture
    ID3D12Resource* dstResource = d.swapchainImages[eyeIndex][imgIndex].texture;
    ID3D12Resource* srcResource = getD3D12Resource(srcTexture);
    if (dstResource && srcResource && d.copyCmdList && d.copyAllocator && d.graphicsQueue) {
        d.copyAllocator->Reset();
        d.copyCmdList->Reset(d.copyAllocator, nullptr);

        // Transition source: RENDER_TARGET -> COPY_SOURCE
        D3D12_RESOURCE_BARRIER barriers[2] = {};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition.pResource = srcResource;
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        // Transition dest: RENDER_TARGET -> COPY_DEST
        barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[1].Transition.pResource = dstResource;
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        d.copyCmdList->ResourceBarrier(2, barriers);

        D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
        dstLoc.pResource = dstResource;
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLoc.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
        srcLoc.pResource = srcResource;
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        srcLoc.SubresourceIndex = 0;

        d.copyCmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

        // Transition back
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        d.copyCmdList->ResourceBarrier(2, barriers);

        d.copyCmdList->Close();

        ID3D12CommandList* cmdLists[] = { d.copyCmdList };
        d.graphicsQueue->ExecuteCommandLists(1, cmdLists);

        d.copyFenceValue++;
        d.graphicsQueue->Signal(d.copyFence, d.copyFenceValue);
        if (d.copyFence->GetCompletedValue() < d.copyFenceValue) {
            d.copyFence->SetEventOnCompletion(d.copyFenceValue, d.copyFenceEvent);
            WaitForSingleObject(d.copyFenceEvent, INFINITE);
        }
    }

    // Release swapchain image
    XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(d.swapchains[eyeIndex], &releaseInfo);

    // Set up projection view for this eye
    d.projectionViews[eyeIndex] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
    d.projectionViews[eyeIndex].pose = d.views[eyeIndex].pose;
    d.projectionViews[eyeIndex].fov = d.views[eyeIndex].fov;
    d.projectionViews[eyeIndex].subImage.swapchain = d.swapchains[eyeIndex];
    d.projectionViews[eyeIndex].subImage.imageRect.offset = {0, 0};
    d.projectionViews[eyeIndex].subImage.imageRect.extent = {d.eyeWidth, d.eyeHeight};
}

int WickedVRSession::getViewCount() const { return (int)pImpl->viewCount; }
int WickedVRSession::getEyeWidth() const { return pImpl->eyeWidth; }
int WickedVRSession::getEyeHeight() const { return pImpl->eyeHeight; }

Vec3 WickedVRSession::getEyePosition(int eyeIndex) const {
    if (eyeIndex < 0 || eyeIndex >= (int)pImpl->viewCount) return {};
    const auto& p = pImpl->views[eyeIndex].pose.position;
    return { p.x, p.y, p.z };
}

Quaternion WickedVRSession::getEyeOrientation(int eyeIndex) const {
    if (eyeIndex < 0 || eyeIndex >= (int)pImpl->viewCount) return {0, 0, 0, 1};
    const auto& q = pImpl->views[eyeIndex].pose.orientation;
    return { q.x, q.y, q.z, q.w };
}

void WickedVRSession::getEyeFov(int eyeIndex, float& left, float& right,
                                  float& up, float& down) const {
    if (eyeIndex < 0 || eyeIndex >= (int)pImpl->viewCount) {
        left = right = up = down = 0;
        return;
    }
    const auto& fov = pImpl->views[eyeIndex].fov;
    left = fov.angleLeft;
    right = fov.angleRight;
    up = fov.angleUp;
    down = fov.angleDown;
}

const VRHandState& WickedVRSession::getHandState(int handIndex) const {
    return pImpl->handStates[handIndex & 1];
}

bool WickedVRSession::isRunning() const { return pImpl->sessionRunning; }

void WickedVRSession::hapticPulse(int handIndex, float amplitude, float durationSec) {
    auto& d = *pImpl;
    if (handIndex < 0 || handIndex > 1 || !d.sessionRunning) return;

    XrPath handPaths[2];
    xrStringToPath(d.instance, "/user/hand/left", &handPaths[0]);
    xrStringToPath(d.instance, "/user/hand/right", &handPaths[1]);

    XrHapticVibration vibration = { XR_TYPE_HAPTIC_VIBRATION };
    vibration.amplitude = amplitude;
    vibration.duration = (XrDuration)(durationSec * 1e9);
    vibration.frequency = XR_FREQUENCY_UNSPECIFIED;

    XrHapticActionInfo hapticInfo = { XR_TYPE_HAPTIC_ACTION_INFO };
    hapticInfo.action = d.hapticAction;
    hapticInfo.subactionPath = handPaths[handIndex];
    xrApplyHapticFeedback(d.session, &hapticInfo, (XrHapticBaseHeader*)&vibration);
}

void WickedVRSession::shutdown() { if (pImpl) pImpl->shutdown(); }

// --- Impl methods ---

bool WickedVRSession::Impl::createInstance() {
    uint32_t extCount = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
    std::vector<XrExtensionProperties> exts(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data());

    bool hasD3D12 = false;
    for (const auto& ext : exts) {
        if (std::string(ext.extensionName) == XR_KHR_D3D12_ENABLE_EXTENSION_NAME) {
            hasD3D12 = true;
            break;
        }
    }
    if (!hasD3D12) {
        std::cerr << "WickedVRSession: XR_KHR_d3d12_enable not available" << std::endl;
        return false;
    }

    const char* extensions[] = { XR_KHR_D3D12_ENABLE_EXTENSION_NAME };

    XrInstanceCreateInfo createInfo = { XR_TYPE_INSTANCE_CREATE_INFO };
    std::strncpy(createInfo.applicationInfo.applicationName, "Bridge Command", XR_MAX_APPLICATION_NAME_SIZE);
    createInfo.applicationInfo.applicationVersion = 6;
    createInfo.applicationInfo.engineVersion = 1;
    std::strncpy(createInfo.applicationInfo.engineName, "WickedEngine", XR_MAX_ENGINE_NAME_SIZE);
    createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    createInfo.enabledExtensionCount = 1;
    createInfo.enabledExtensionNames = extensions;

    XrResult result = xrCreateInstance(&createInfo, &instance);
    if (!xrCheck(XR_NULL_HANDLE, result, "xrCreateInstance")) return false;

    XrSystemGetInfo systemInfo = { XR_TYPE_SYSTEM_GET_INFO };
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    result = xrGetSystem(instance, &systemInfo, &systemId);
    if (!xrCheck(instance, result, "xrGetSystem")) {
        std::cerr << "WickedVRSession: No VR headset found" << std::endl;
        return false;
    }

    uint32_t configCount = 0;
    xrEnumerateViewConfigurationViews(instance, systemId,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &configCount, nullptr);
    viewConfigViews.resize(configCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(instance, systemId,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, configCount, &configCount,
        viewConfigViews.data());

    viewCount = configCount;
    if (viewCount < 2) {
        std::cerr << "WickedVRSession: Expected 2 views, got " << viewCount << std::endl;
        return false;
    }

    eyeWidth = (int)viewConfigViews[0].recommendedImageRectWidth;
    eyeHeight = (int)viewConfigViews[0].recommendedImageRectHeight;
    views.resize(viewCount, {XR_TYPE_VIEW});

    return true;
}

bool WickedVRSession::Impl::createSession() {
    auto* dx12 = getDX12Device();
    if (!dx12) {
        std::cerr << "WickedVRSession: WE is not using DX12 backend" << std::endl;
        return false;
    }

    ID3D12CommandQueue* cmdQueue = dx12->GetGraphicsCommandQueue();
    if (!cmdQueue) {
        std::cerr << "WickedVRSession: Cannot access D3D12 command queue" << std::endl;
        return false;
    }

    ID3D12Device* d3dDevice = nullptr;
    cmdQueue->GetDevice(IID_PPV_ARGS(&d3dDevice));
    if (!d3dDevice) {
        std::cerr << "WickedVRSession: Cannot access D3D12 device" << std::endl;
        return false;
    }

    // Check D3D12 requirements
    XrGraphicsRequirementsD3D12KHR d3d12Reqs = { XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR };
    PFN_xrGetD3D12GraphicsRequirementsKHR pfnGetD3D12Reqs = nullptr;
    xrGetInstanceProcAddr(instance, "xrGetD3D12GraphicsRequirementsKHR",
        (PFN_xrVoidFunction*)&pfnGetD3D12Reqs);
    if (pfnGetD3D12Reqs) {
        pfnGetD3D12Reqs(instance, systemId, &d3d12Reqs);
    }

    XrGraphicsBindingD3D12KHR d3d12Binding = { XR_TYPE_GRAPHICS_BINDING_D3D12_KHR };
    d3d12Binding.device = d3dDevice;
    d3d12Binding.queue = cmdQueue;

    XrSessionCreateInfo sessionInfo = { XR_TYPE_SESSION_CREATE_INFO };
    sessionInfo.next = &d3d12Binding;
    sessionInfo.systemId = systemId;

    XrResult result = xrCreateSession(instance, &sessionInfo, &session);

    // Create D3D12 copy infrastructure before releasing device ref
    graphicsQueue = cmdQueue;
    {
        HRESULT hr = d3dDevice->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&copyAllocator));
        if (FAILED(hr)) {
            std::cerr << "WickedVRSession: Failed to create copy command allocator" << std::endl;
            d3dDevice->Release();
            return false;
        }
        hr = d3dDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            copyAllocator, nullptr, IID_PPV_ARGS(&copyCmdList));
        if (FAILED(hr)) {
            std::cerr << "WickedVRSession: Failed to create copy command list" << std::endl;
            d3dDevice->Release();
            return false;
        }
        copyCmdList->Close();

        hr = d3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copyFence));
        if (FAILED(hr)) {
            std::cerr << "WickedVRSession: Failed to create copy fence" << std::endl;
            d3dDevice->Release();
            return false;
        }
        copyFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        copyFenceValue = 0;
    }

    d3dDevice->Release();

    if (!xrCheck(instance, result, "xrCreateSession")) return false;

    XrReferenceSpaceCreateInfo spaceInfo = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
    result = xrCreateReferenceSpace(session, &spaceInfo, &playSpace);
    if (!xrCheck(instance, result, "xrCreateReferenceSpace")) return false;

    return true;
}

bool WickedVRSession::Impl::createSwapchains() {
    swapchains.resize(viewCount);
    swapchainImages.resize(viewCount);
    swapchainLengths.resize(viewCount);

    uint32_t fmtCount = 0;
    xrEnumerateSwapchainFormats(session, 0, &fmtCount, nullptr);
    std::vector<int64_t> formats(fmtCount);
    xrEnumerateSwapchainFormats(session, fmtCount, &fmtCount, formats.data());

    int64_t chosenFormat = (int64_t)DXGI_FORMAT_R8G8B8A8_UNORM;
    for (int64_t fmt : formats) {
        if (fmt == (int64_t)DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
            chosenFormat = fmt;
            break;
        }
    }

    for (uint32_t i = 0; i < viewCount; i++) {
        XrSwapchainCreateInfo swapchainInfo = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                                    XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        swapchainInfo.format = chosenFormat;
        swapchainInfo.sampleCount = 1;
        swapchainInfo.width = eyeWidth;
        swapchainInfo.height = eyeHeight;
        swapchainInfo.faceCount = 1;
        swapchainInfo.arraySize = 1;
        swapchainInfo.mipCount = 1;

        XrResult result = xrCreateSwapchain(session, &swapchainInfo, &swapchains[i]);
        if (!xrCheck(instance, result, "xrCreateSwapchain")) return false;

        uint32_t imgCount = 0;
        xrEnumerateSwapchainImages(swapchains[i], 0, &imgCount, nullptr);
        swapchainLengths[i] = imgCount;
        swapchainImages[i].resize(imgCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        xrEnumerateSwapchainImages(swapchains[i], imgCount, &imgCount,
            (XrSwapchainImageBaseHeader*)swapchainImages[i].data());
    }

    return true;
}

bool WickedVRSession::Impl::createActions() {
    XrActionSetCreateInfo setInfo = { XR_TYPE_ACTION_SET_CREATE_INFO };
    std::strncpy(setInfo.actionSetName, "gameplay", XR_MAX_ACTION_SET_NAME_SIZE);
    std::strncpy(setInfo.localizedActionSetName, "Gameplay", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE);
    XrResult result = xrCreateActionSet(instance, &setInfo, &actionSet);
    if (!xrCheck(instance, result, "xrCreateActionSet")) return false;

    XrPath handPaths[2];
    xrStringToPath(instance, "/user/hand/left", &handPaths[HAND_LEFT]);
    xrStringToPath(instance, "/user/hand/right", &handPaths[HAND_RIGHT]);

    auto createAction = [&](XrActionType type, const char* name, const char* localized, XrAction* out) {
        XrActionCreateInfo ai = { XR_TYPE_ACTION_CREATE_INFO };
        ai.actionType = type;
        std::strncpy(ai.actionName, name, XR_MAX_ACTION_NAME_SIZE);
        std::strncpy(ai.localizedActionName, localized, XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
        ai.countSubactionPaths = 2;
        ai.subactionPaths = handPaths;
        xrCreateAction(actionSet, &ai, out);
    };

    createAction(XR_ACTION_TYPE_POSE_INPUT, "grip_pose", "Grip Pose", &gripPoseAction);
    createAction(XR_ACTION_TYPE_POSE_INPUT, "aim_pose", "Aim Pose", &aimPoseAction);
    createAction(XR_ACTION_TYPE_FLOAT_INPUT, "select", "Select", &selectAction);
    createAction(XR_ACTION_TYPE_FLOAT_INPUT, "trigger", "Trigger", &triggerAction);
    createAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", "Menu", &menuAction);
    createAction(XR_ACTION_TYPE_FLOAT_INPUT, "thumbstick_y", "Thumbstick Y", &thumbstickYAction);
    createAction(XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic", "Haptic", &hapticAction);

    // Suggest bindings for simple_controller (select/click + menu/click only)
    XrPath profilePath;
    xrStringToPath(instance, "/interaction_profiles/khr/simple_controller", &profilePath);

    XrPath bindPaths[10];
    xrStringToPath(instance, "/user/hand/left/input/grip/pose", &bindPaths[0]);
    xrStringToPath(instance, "/user/hand/right/input/grip/pose", &bindPaths[1]);
    xrStringToPath(instance, "/user/hand/left/input/aim/pose", &bindPaths[2]);
    xrStringToPath(instance, "/user/hand/right/input/aim/pose", &bindPaths[3]);
    xrStringToPath(instance, "/user/hand/left/input/select/click", &bindPaths[4]);
    xrStringToPath(instance, "/user/hand/right/input/select/click", &bindPaths[5]);
    xrStringToPath(instance, "/user/hand/left/input/menu/click", &bindPaths[6]);
    xrStringToPath(instance, "/user/hand/right/input/menu/click", &bindPaths[7]);
    xrStringToPath(instance, "/user/hand/left/output/haptic", &bindPaths[8]);
    xrStringToPath(instance, "/user/hand/right/output/haptic", &bindPaths[9]);

    XrActionSuggestedBinding bindings[] = {
        {gripPoseAction, bindPaths[0]}, {gripPoseAction, bindPaths[1]},
        {aimPoseAction, bindPaths[2]},  {aimPoseAction, bindPaths[3]},
        {selectAction, bindPaths[4]},   {selectAction, bindPaths[5]},
        {menuAction, bindPaths[6]},     {menuAction, bindPaths[7]},
        {hapticAction, bindPaths[8]},   {hapticAction, bindPaths[9]},
    };

    XrInteractionProfileSuggestedBinding suggestedBindings = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    suggestedBindings.interactionProfile = profilePath;
    suggestedBindings.suggestedBindings = bindings;
    suggestedBindings.countSuggestedBindings = sizeof(bindings) / sizeof(bindings[0]);
    xrSuggestInteractionProfileBindings(instance, &suggestedBindings);

    for (int h = 0; h < 2; h++) {
        XrActionSpaceCreateInfo spaceInfo = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
        spaceInfo.action = gripPoseAction;
        spaceInfo.subactionPath = handPaths[h];
        spaceInfo.poseInActionSpace.orientation.w = 1.0f;
        xrCreateActionSpace(session, &spaceInfo, &gripPoseSpaces[h]);

        spaceInfo.action = aimPoseAction;
        xrCreateActionSpace(session, &spaceInfo, &aimPoseSpaces[h]);
    }

    XrSessionActionSetsAttachInfo attachInfo = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    attachInfo.countActionSets = 1;
    attachInfo.actionSets = &actionSet;
    xrAttachSessionActionSets(session, &attachInfo);

    return true;
}

void WickedVRSession::Impl::pollEvents() {
    XrEventDataBuffer event = { XR_TYPE_EVENT_DATA_BUFFER };
    while (xrPollEvent(instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* stateEvent = (XrEventDataSessionStateChanged*)&event;
            sessionState = stateEvent->state;
            switch (sessionState) {
            case XR_SESSION_STATE_READY: {
                XrSessionBeginInfo beginInfo = { XR_TYPE_SESSION_BEGIN_INFO };
                beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                xrBeginSession(session, &beginInfo);
                sessionRunning = true;
                std::cout << "WickedVRSession: Session started" << std::endl;
                break;
            }
            case XR_SESSION_STATE_STOPPING:
                xrEndSession(session);
                sessionRunning = false;
                std::cout << "WickedVRSession: Session stopped" << std::endl;
                break;
            case XR_SESSION_STATE_EXITING:
            case XR_SESSION_STATE_LOSS_PENDING:
                sessionRunning = false;
                break;
            default: break;
            }
        }
        event = { XR_TYPE_EVENT_DATA_BUFFER };
    }
}

void WickedVRSession::Impl::syncActions() {
    XrActiveActionSet activeSet = {};
    activeSet.actionSet = actionSet;
    XrActionsSyncInfo syncInfo = { XR_TYPE_ACTIONS_SYNC_INFO };
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &activeSet;
    xrSyncActions(session, &syncInfo);

    XrPath handPaths[2];
    xrStringToPath(instance, "/user/hand/left", &handPaths[HAND_LEFT]);
    xrStringToPath(instance, "/user/hand/right", &handPaths[HAND_RIGHT]);

    for (int h = 0; h < 2; h++) {
        auto& hs = handStates[h];

        XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
        xrLocateSpace(gripPoseSpaces[h], playSpace, frameState.predictedDisplayTime, &loc);
        if (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) {
            hs.gripPosition = { loc.pose.position.x, loc.pose.position.y, loc.pose.position.z };
            hs.gripOrientation = { loc.pose.orientation.x, loc.pose.orientation.y,
                                   loc.pose.orientation.z, loc.pose.orientation.w };
            hs.poseValid = true;
        } else {
            hs.poseValid = false;
        }

        XrSpaceLocation aimLoc = { XR_TYPE_SPACE_LOCATION };
        xrLocateSpace(aimPoseSpaces[h], playSpace, frameState.predictedDisplayTime, &aimLoc);
        if (aimLoc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) {
            hs.aimPosition = { aimLoc.pose.position.x, aimLoc.pose.position.y, aimLoc.pose.position.z };
            hs.aimOrientation = { aimLoc.pose.orientation.x, aimLoc.pose.orientation.y,
                                  aimLoc.pose.orientation.z, aimLoc.pose.orientation.w };
        }

        XrActionStateFloat floatState = { XR_TYPE_ACTION_STATE_FLOAT };
        XrActionStateGetInfo getInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
        getInfo.subactionPath = handPaths[h];

        getInfo.action = selectAction;
        xrGetActionStateFloat(session, &getInfo, &floatState);
        hs.selectValue = floatState.currentState;

        getInfo.action = triggerAction;
        floatState = { XR_TYPE_ACTION_STATE_FLOAT };
        xrGetActionStateFloat(session, &getInfo, &floatState);
        hs.triggerValue = floatState.currentState;

        getInfo.action = thumbstickYAction;
        floatState = { XR_TYPE_ACTION_STATE_FLOAT };
        xrGetActionStateFloat(session, &getInfo, &floatState);
        hs.thumbstickY = floatState.currentState;

        XrActionStateBoolean boolState = { XR_TYPE_ACTION_STATE_BOOLEAN };
        getInfo.action = menuAction;
        xrGetActionStateBoolean(session, &getInfo, &boolState);
        hs.menuPressed = boolState.currentState;
    }
}

void WickedVRSession::Impl::shutdown() {
    if (session) {
        if (sessionRunning) {
            xrRequestExitSession(session);
            for (int i = 0; i < 100 && sessionRunning; i++) {
                pollEvents();
            }
        }

        for (auto& sc : swapchains) {
            if (sc) xrDestroySwapchain(sc);
        }
        swapchains.clear();
        swapchainImages.clear();

        for (int h = 0; h < 2; h++) {
            if (gripPoseSpaces[h]) xrDestroySpace(gripPoseSpaces[h]);
            if (aimPoseSpaces[h]) xrDestroySpace(aimPoseSpaces[h]);
            gripPoseSpaces[h] = XR_NULL_HANDLE;
            aimPoseSpaces[h] = XR_NULL_HANDLE;
        }
        if (actionSet) xrDestroyActionSet(actionSet);
        actionSet = XR_NULL_HANDLE;

        if (playSpace) xrDestroySpace(playSpace);
        playSpace = XR_NULL_HANDLE;

        xrDestroySession(session);
        session = XR_NULL_HANDLE;
    }

    // Release D3D12 copy resources
    if (copyFenceEvent) { CloseHandle(copyFenceEvent); copyFenceEvent = nullptr; }
    if (copyFence) { copyFence->Release(); copyFence = nullptr; }
    if (copyCmdList) { copyCmdList->Release(); copyCmdList = nullptr; }
    if (copyAllocator) { copyAllocator->Release(); copyAllocator = nullptr; }
    graphicsQueue = nullptr;

    if (instance) {
        xrDestroyInstance(instance);
        instance = XR_NULL_HANDLE;
    }

    sessionRunning = false;
}

}}} // namespace bc::graphics::wicked

#endif // _WIN64
#endif // WITH_WICKED_ENGINE
