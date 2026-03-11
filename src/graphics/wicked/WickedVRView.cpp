/*   Bridge Command 5.0 Ship Simulator
     Copyright (C) 2026 James Packer

     This program is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License version 2 as
     published by the Free Software Foundation */

#ifdef WITH_WICKED_ENGINE

#include "WickedVRView.hpp"
#include <iostream>
#include <cmath>
#include <cstring>

namespace bc { namespace graphics { namespace wicked {

WickedVRView::~WickedVRView() {
    shutdown();
}

void WickedVRView::init(wi::scene::Scene* scene, int eyeWidth, int eyeHeight) {
    if (initialized) {
        shutdown();
    }

    weScene = scene;

    wi::graphics::GraphicsDevice* device = wi::graphics::GetDevice();

    for (int eye = 0; eye < EYE_COUNT; ++eye) {
        auto& ev = eyes[eye];
        ev.width = eyeWidth;
        ev.height = eyeHeight;

        // Create a standalone camera component (not entity-based, like MultiView)
        ev.camera = std::make_unique<wi::scene::CameraComponent>();
        ev.camera->zNearP = nearPlane;
        ev.camera->zFarP = farPlane;
        ev.camera->width = (float)eyeWidth;
        ev.camera->height = (float)eyeHeight;

        // Also create an entity-based camera for scene queries
        std::string name = (eye == EYE_LEFT) ? "BC_VR_LeftEye" : "BC_VR_RightEye";
        ev.cameraEntity = weScene->Entity_CreateCamera(
            name, eyeWidth, eyeHeight, nearPlane, farPlane);

        // Create render target texture for this eye
        wi::graphics::TextureDesc desc;
        desc.width = eyeWidth;
        desc.height = eyeHeight;
        desc.format = wi::graphics::Format::R8G8B8A8_UNORM;
        desc.bind_flags = wi::graphics::BindFlag::RENDER_TARGET |
                          wi::graphics::BindFlag::SHADER_RESOURCE;
        desc.usage = wi::graphics::Usage::DEFAULT;
        desc.sample_count = 1;
        desc.mip_levels = 1;
        desc.array_size = 1;
        desc.layout = wi::graphics::ResourceState::RENDERTARGET;

        bool ok = device->CreateTexture(&desc, nullptr, &ev.renderTarget);
        if (!ok) {
            std::cerr << "WickedVRView: Failed to create render target for eye " << eye << std::endl;
            continue;
        }

        // Create depth target
        wi::graphics::TextureDesc depthDesc;
        depthDesc.width = eyeWidth;
        depthDesc.height = eyeHeight;
        depthDesc.format = wi::graphics::Format::D32_FLOAT;
        depthDesc.bind_flags = wi::graphics::BindFlag::DEPTH_STENCIL;
        depthDesc.usage = wi::graphics::Usage::DEFAULT;
        depthDesc.sample_count = 1;
        depthDesc.mip_levels = 1;
        depthDesc.array_size = 1;
        depthDesc.layout = wi::graphics::ResourceState::DEPTHSTENCIL;

        ok = device->CreateTexture(&depthDesc, nullptr, &ev.depthTarget);
        if (!ok) {
            std::cerr << "WickedVRView: Failed to create depth target for eye " << eye << std::endl;
            continue;
        }

        // Create RenderPath3D for this eye (same pattern as WickedMultiView)
        ev.renderPath = std::make_unique<wi::RenderPath3D>();
        ev.renderPath->init(eyeWidth, eyeHeight);
        ev.renderPath->scene = weScene;
        ev.renderPath->camera = ev.camera.get();
        ev.renderPath->setSceneUpdateEnabled(false); // Main path updates the scene

        // VR quality: favor performance over quality for 90fps target
        ev.renderPath->setSSREnabled(false);        // SSR is expensive
        ev.renderPath->setFXAAEnabled(true);
        ev.renderPath->setBloomEnabled(true);
        ev.renderPath->setLensFlareEnabled(false);   // Distracting in VR
        ev.renderPath->setAO(wi::RenderPath3D::AO_HBAO);
        ev.renderPath->setAORange(1.5f);
        ev.renderPath->setAOPower(1.5f);
        ev.renderPath->setEyeAdaptionEnabled(true);
        ev.renderPath->setEyeAdaptionKey(0.08f);
        ev.renderPath->setLightShaftsEnabled(true);
        ev.renderPath->setLightShaftsStrength(0.03f);
        ev.renderPath->setExposure(1.1f);
        ev.renderPath->setSharpenFilterEnabled(false); // Can cause shimmer in VR
        ev.renderPath->setDitherEnabled(true);

        ev.active = true;
    }

    initialized = true;
    std::cout << "WickedVRView: Initialized stereo rendering ("
              << eyeWidth << "x" << eyeHeight << " per eye)" << std::endl;
}

void WickedVRView::updateEyePose(int eyeIndex,
                                  const Vec3& position,
                                  const Quaternion& orientation,
                                  float fovLeft, float fovRight,
                                  float fovUp, float fovDown) {
    if (eyeIndex < 0 || eyeIndex >= EYE_COUNT || !initialized) return;

    auto& ev = eyes[eyeIndex];
    if (!ev.active || !ev.camera) return;

    auto* cam = ev.camera.get();

    // Set eye position
    cam->Eye = XMFLOAT3(position.x, position.y, position.z);

    // Convert quaternion to rotation matrix for forward/up extraction
    // OpenXR: right-handed. WE: left-handed. Caller must Z-flip poses.
    float qx = orientation.x, qy = orientation.y, qz = orientation.z, qw = orientation.w;
    float xx = qx * qx, yy = qy * qy, zz = qz * qz;
    float xy = qx * qy, xz = qx * qz, xw = qx * qw;
    float yz = qy * qz, yw = qy * qw, zw = qz * qw;

    // Forward = -Z column of rotation matrix
    XMFLOAT3 forward;
    forward.x = -(2.0f * (xz + yw));
    forward.y = -(2.0f * (yz - xw));
    forward.z = -(1.0f - 2.0f * (xx + yy));

    cam->At = XMFLOAT3(
        cam->Eye.x + forward.x,
        cam->Eye.y + forward.y,
        cam->Eye.z + forward.z);

    // Up = Y column of rotation matrix
    cam->Up = XMFLOAT3(
        2.0f * (xy - zw),
        1.0f - 2.0f * (xx + zz),
        2.0f * (yz + xw));

    // Build asymmetric projection from OpenXR FOV angles
    float tanLeft = std::tan(fovLeft);
    float tanRight = std::tan(fovRight);
    float tanUp = std::tan(fovUp);
    float tanDown = std::tan(fovDown);

    float nearZ = nearPlane;
    float farZ = farPlane;
    float invWidth = 1.0f / (tanRight - tanLeft);
    float invHeight = 1.0f / (tanUp - tanDown);
    float range = farZ / (farZ - nearZ);

    XMFLOAT4X4 proj;
    std::memset(&proj, 0, sizeof(proj));
    proj._11 = 2.0f * invWidth;
    proj._22 = 2.0f * invHeight;
    proj._31 = -(tanRight + tanLeft) * invWidth;
    proj._32 = -(tanUp + tanDown) * invHeight;
    proj._33 = range;
    proj._34 = 1.0f;
    proj._43 = -range * nearZ;

    cam->Projection = proj;
    cam->SetDirty();

    // Also update the view matrix via TransformCamera for WE internal use
    DirectX::XMVECTOR vEye = DirectX::XMVectorSet(position.x, position.y, position.z, 1.0f);
    DirectX::XMVECTOR vAt = DirectX::XMVectorSet(cam->At.x, cam->At.y, cam->At.z, 1.0f);
    DirectX::XMVECTOR vUp = DirectX::XMVectorSet(cam->Up.x, cam->Up.y, cam->Up.z, 0.0f);
    DirectX::XMMATRIX viewMat = DirectX::XMMatrixLookAtLH(vEye, vAt, vUp);
    DirectX::XMMATRIX invView = DirectX::XMMatrixInverse(nullptr, viewMat);
    cam->TransformCamera(invView);

    // Override projection after TransformCamera (it recomputes from fov)
    cam->Projection = proj;
}

void WickedVRView::renderEye(int eyeIndex) {
    if (eyeIndex < 0 || eyeIndex >= EYE_COUNT || !initialized) return;

    auto& ev = eyes[eyeIndex];
    if (!ev.active || !ev.renderPath) return;

    // Run the full render pipeline for this eye (same sequence as WickedMultiView)
    ev.renderPath->init(ev.width, ev.height);
    ev.renderPath->PreUpdate();
    ev.renderPath->Update(0);
    ev.renderPath->PostUpdate();
    ev.renderPath->PreRender();
    ev.renderPath->Render();
    ev.renderPath->PostRender();

    // Compose the final image into our render target
    auto* device = wi::graphics::GetDevice();
    wi::graphics::CommandList cmd = device->BeginCommandList();
    wi::graphics::Viewport viewport;
    viewport.width = (float)ev.width;
    viewport.height = (float)ev.height;
    device->BindViewports(1, &viewport, cmd);

    wi::graphics::RenderPassImage rpImages[] = {
        wi::graphics::RenderPassImage::RenderTarget(&ev.renderTarget,
            wi::graphics::RenderPassImage::LoadOp::CLEAR),
    };
    device->RenderPassBegin(rpImages, 1, cmd);
    ev.renderPath->Compose(cmd);
    device->RenderPassEnd(cmd);
    device->SubmitCommandLists();
}

const wi::graphics::Texture* WickedVRView::getEyeTexture(int eyeIndex) const {
    if (eyeIndex < 0 || eyeIndex >= EYE_COUNT || !initialized) return nullptr;
    if (!eyes[eyeIndex].active) return nullptr;
    return &eyes[eyeIndex].renderTarget;
}

wi::scene::CameraComponent* WickedVRView::getCamera(int eyeIndex) {
    if (eyeIndex < 0 || eyeIndex >= EYE_COUNT || !initialized) return nullptr;
    return eyes[eyeIndex].camera.get();
}

void WickedVRView::setClipPlanes(float nearZ, float farZ) {
    nearPlane = nearZ;
    farPlane = farZ;
}

void WickedVRView::shutdown() {
    if (!initialized) return;

    for (int eye = 0; eye < EYE_COUNT; ++eye) {
        auto& ev = eyes[eye];
        ev.renderPath.reset();
        ev.camera.reset();
        if (ev.cameraEntity != wi::ecs::INVALID_ENTITY && weScene) {
            weScene->Entity_Remove(ev.cameraEntity);
            ev.cameraEntity = wi::ecs::INVALID_ENTITY;
        }
        ev.renderTarget = {};
        ev.depthTarget = {};
        ev.active = false;
    }

    weScene = nullptr;
    initialized = false;
}

}}} // namespace bc::graphics::wicked

#endif // WITH_WICKED_ENGINE
