#ifdef WITH_WICKED_ENGINE

#include "WickedMain.hpp"

// WickedEngine must be included before any Irrlicht headers
#include "WickedEngine.h"

#include "graphics/wicked/WickedMultiCascadeOcean.hpp"
#include "graphics/wicked/WickedTerrainNode.hpp"
#include "graphics/wicked/WickedModelImporter.hpp"
#include "graphics/wicked/WickedImGui.hpp"
#include "gui/ImGuiOverlay.hpp"
#include "gui/SettingsPanel.hpp"
#include "gui/RadarDisplay.hpp"
#include "gui/EcdisDisplay.hpp"
#include "IrrlichtModelConverter.hpp"
#include "SimulationBridge.hpp"
#include "BuildingGenerator.hpp"
#include "editor/OSMBuildingReader.hpp"
#include "IniFile.hpp"
#include "Utilities.hpp"
#include "Constants.hpp"
#include "Sound.hpp"
#include "TextureUpscaler.hpp"
#include "MapScreen.hpp"

// ImGui header needed for IO access in game loop
#include "graphics/wicked/imgui/imgui.h"

// Win32 platform backend for ImGui (mouse/keyboard/scroll input)
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
bool ImGui_ImplWin32_Init(void* hwnd);
void ImGui_ImplWin32_Shutdown();
void ImGui_ImplWin32_NewFrame();

#include <iostream>
#include <fstream>
#include <cmath>
#include <cstdlib>
#include <vector>
#include <string>
#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <sstream>

// Log file for diagnosing crashes (console disappears on crash)
static std::ofstream g_weLog;
static void weLog(const std::string& msg) {
    if (g_weLog.is_open()) {
        g_weLog << msg << std::endl;
        g_weLog.flush();
    }
    std::cout << msg << std::endl;
}
static void weLogErr(const std::string& msg) {
    if (g_weLog.is_open()) {
        g_weLog << "ERROR: " << msg << std::endl;
        g_weLog.flush();
    }
    std::cerr << "ERROR: " << msg << std::endl;
}

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Pump Win32 messages to keep the window responsive during long operations
static void pumpMessages() {
    MSG msg = {};
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

// SEH wrapper: runs application.Run() with structured exception handling
// Must be in a separate function since __try cannot coexist with C++ try/catch
static DWORD g_lastSEHCode = 0;
static bool runAppWithSEH(wi::Application& app) {
    __try {
        app.Run();
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        g_lastSEHCode = GetExceptionCode();
        return false;
    }
    return true;
}

// Wicked Engine application instance (must be global for WndProc access)
static wi::Application* g_weApp = nullptr;

static LRESULT CALLBACK WickedWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    // Forward to ImGui Win32 backend for mouse/keyboard/scroll input
    if (ImGui_ImplWin32_WndProcHandler(hWnd, message, wParam, lParam))
        return true;

    switch (message) {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) {
            if (g_weApp) g_weApp->is_window_active = false;
        } else if (wParam == SIZE_RESTORED || wParam == SIZE_MAXIMIZED) {
            if (g_weApp) {
                g_weApp->is_window_active = true;
                g_weApp->SetWindow(hWnd);
            }
        }
        break;
    case WM_DPICHANGED:
        if (g_weApp && g_weApp->is_window_active)
            g_weApp->SetWindow(hWnd);
        break;
    case WM_CHAR:
        switch (wParam) {
        case VK_BACK:
            wi::gui::TextInputField::DeleteFromInput();
            break;
        default: {
            const wchar_t c = (const wchar_t)wParam;
            wi::gui::TextInputField::AddInput(c);
        } break;
        }
        break;
    case WM_INPUT:
        wi::input::rawinput::ParseMessage((void*)lParam);
        break;
    case WM_KILLFOCUS:
        if (g_weApp) g_weApp->is_window_active = false;
        break;
    case WM_SETFOCUS:
        if (g_weApp) g_weApp->is_window_active = true;
        break;
    case WM_DESTROY:
        weLog("WM_DESTROY received -- posting quit");
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

// Camera state -- first-person bridge view by default, 'O' toggles orbit
static bool camOrbitMode = false;
static float camDistance = 200.0f;
static float camYaw = 0.0f;    // horizontal look angle (degrees, CW from north)
static float camYawOffset = 0.0f; // mouse look offset from ship heading (bridge mode)
static float camPitch = 0.0f;  // vertical look angle (degrees, + = up)
static float camPosX = 0.0f;   // camera world position (WE coords)
static float camPosY = 10.0f;
static float camPosZ = 0.0f;
static float camTargetX = 0.0f;
static float camTargetY = 10.0f;
static float camTargetZ = 0.0f;
static float walkLocalX = 0.0f;  // ship-local walk offset from default bridge position
static float walkLocalZ = 0.0f;
static bool mouseRightDown = false;
static bool mouseLeftDown = false;
static int lastMouseX = 0, lastMouseY = 0;

// Persistent radar texture for rendering (must be before BCRenderPath)
static wi::graphics::Texture g_radarTex;

// RenderPath subclass to hook ImGui rendering into WE's Compose pass
class BCRenderPath : public wi::RenderPath3D {
public:
    void Compose(wi::graphics::CommandList cmd) const override {
        wi::RenderPath3D::Compose(cmd);
        bc::graphics::wicked::ImGuiRender(cmd);
    }
};

// Coordinate conversion: replicates Terrain::longToX() / latToZ()
struct CoordConverter {
    float terrainLong = 0;
    float terrainLat = 0;
    float terrainLongExtent = 0;
    float terrainLatExtent = 0;
    float terrainXWidth = 0;
    float terrainZWidth = 0;

    void init(float lon, float lat, float lonExtent, float latExtent) {
        terrainLong = lon;
        terrainLat = lat;
        terrainLongExtent = lonExtent;
        terrainLatExtent = latExtent;
        float cosLat = std::cos((lat + latExtent / 2.0f) * (float)M_PI / 180.0f);
        terrainXWidth = lonExtent * 2.0f * (float)M_PI * EARTH_RAD_M * cosLat / 360.0f;
        terrainZWidth = latExtent * 2.0f * (float)M_PI * EARTH_RAD_M / 360.0f;
    }

    float longToX(float longitude) const {
        if (terrainLongExtent == 0) return 0;
        return ((longitude - terrainLong) * terrainXWidth) / terrainLongExtent;
    }
    float latToZ(float latitude) const {
        if (terrainLatExtent == 0) return 0;
        return ((latitude - terrainLat) * terrainZWidth) / terrainLatExtent;
    }
    double xToLong(float x) const {
        if (terrainXWidth == 0) return terrainLong;
        return terrainLong + x * terrainLongExtent / terrainXWidth;
    }
    double zToLat(float z) const {
        if (terrainZWidth == 0) return terrainLat;
        return terrainLat + z * terrainLatExtent / terrainZWidth;
    }
};

// ===== Procedural lighthouse geometry =====
// Creates a multi-section cylindrical lighthouse with tower, gallery, lantern, and dome.
// Cached by height (rounded to nearest 0.5m) to share GPU mesh across instances.
static std::unordered_map<int, wi::ecs::Entity> g_lighthouseMeshCache;

static wi::ecs::Entity createProceduralLighthouse(wi::scene::Scene& scene,
                                                    const std::string& name,
                                                    float totalHeight) {
    // Clamp to reasonable range; default 15m if not provided
    if (totalHeight < 3.0f) totalHeight = 15.0f;
    if (totalHeight > 80.0f) totalHeight = 80.0f;

    // Cache by height (0.5m granularity)
    int cacheKey = (int)(totalHeight * 2);
    auto it = g_lighthouseMeshCache.find(cacheKey);
    if (it != g_lighthouseMeshCache.end()) {
        wi::ecs::Entity entity = scene.Entity_CreateObject(name);
        auto* object = scene.objects.GetComponent(entity);
        if (object) object->meshID = it->second;
        return entity;
    }

    const int N = 16; // circumference segments

    // Section boundaries (absolute heights)
    float towerTop   = totalHeight - 3.5f;
    float galleryTop = totalHeight - 3.0f;
    float lanternTop = totalHeight - 0.8f;
    float domeTop    = totalHeight;

    // Radii (proportional with reasonable minimums)
    float baseR      = std::max(1.5f, totalHeight * 0.10f);
    float towerTopR  = baseR * 0.75f;
    float galleryR   = baseR * 1.3f;
    float lanternR   = towerTopR * 0.7f;

    wi::ecs::Entity meshEntity = scene.Entity_CreateMesh(name + "_lh_mesh");
    auto* mesh = scene.meshes.GetComponent(meshEntity);
    if (!mesh) return wi::ecs::INVALID_ENTITY;

    // Lambda: add a cylinder/cone section with optional top cap
    int sectionIdx = 0;
    auto addSection = [&](float yBot, float yTop, float rBot, float rTop,
                          float cr, float cg, float cb, float rough, bool cap) {
        wi::ecs::Entity matEntity = scene.Entity_CreateMaterial(name + "_lh_mat" + std::to_string(sectionIdx++));
        auto* mat = scene.materials.GetComponent(matEntity);
        if (mat) {
            mat->baseColor = DirectX::XMFLOAT4(cr, cg, cb, 1.0f);
            mat->roughness = rough;
            mat->metalness = 0.0f;
            mat->SetDoubleSided(true);
            mat->CreateRenderData();
        }

        uint32_t base = (uint32_t)mesh->vertex_positions.size();

        for (int i = 0; i <= N; i++) {
            float a = (float)i / N * 6.28318530718f;
            float ca = cosf(a), sa = sinf(a);

            // Bottom ring vertex
            mesh->vertex_positions.push_back({rBot * ca, yBot, rBot * sa});
            mesh->vertex_normals.push_back({ca, 0.0f, sa});
            mesh->vertex_uvset_0.push_back({(float)i / N, 0.0f});

            // Top ring vertex
            mesh->vertex_positions.push_back({rTop * ca, yTop, rTop * sa});
            mesh->vertex_normals.push_back({ca, 0.0f, sa});
            mesh->vertex_uvset_0.push_back({(float)i / N, 1.0f});
        }

        wi::scene::MeshComponent::MeshSubset subset;
        subset.materialID = matEntity;
        subset.indexOffset = (uint32_t)mesh->indices.size();

        // Wall triangles (two per quad segment)
        for (int i = 0; i < N; i++) {
            uint32_t bl = base + i * 2;
            uint32_t tl = bl + 1;
            uint32_t br = base + (i + 1) * 2;
            uint32_t tr = br + 1;

            mesh->indices.push_back(bl);
            mesh->indices.push_back(tl);
            mesh->indices.push_back(br);
            mesh->indices.push_back(br);
            mesh->indices.push_back(tl);
            mesh->indices.push_back(tr);
        }

        // Top cap (fan from center vertex)
        if (cap && rTop > 0.01f) {
            uint32_t center = (uint32_t)mesh->vertex_positions.size();
            mesh->vertex_positions.push_back({0.0f, yTop, 0.0f});
            mesh->vertex_normals.push_back({0.0f, 1.0f, 0.0f});
            mesh->vertex_uvset_0.push_back({0.5f, 0.5f});

            for (int i = 0; i < N; i++) {
                uint32_t t0 = base + i * 2 + 1;       // top ring vertex i
                uint32_t t1 = base + (i + 1) * 2 + 1; // top ring vertex i+1
                mesh->indices.push_back(center);
                mesh->indices.push_back(t0);
                mesh->indices.push_back(t1);
            }
        }

        subset.indexCount = (uint32_t)(mesh->indices.size() - subset.indexOffset);
        mesh->subsets.push_back(subset);
    };

    // Tower body: cream/white, tapered
    addSection(0.0f, towerTop, baseR, towerTopR,  0.95f, 0.92f, 0.85f, 0.65f, false);
    // Gallery platform: dark grey iron, wider, capped
    addSection(towerTop, galleryTop, galleryR, galleryR,  0.20f, 0.20f, 0.22f, 0.40f, true);
    // Lantern room: dark blue-grey glass
    addSection(galleryTop, lanternTop, lanternR, lanternR,  0.12f, 0.15f, 0.20f, 0.25f, false);
    // Dome: red, tapered to near-point
    addSection(lanternTop, domeTop, lanternR, 0.05f,  0.75f, 0.10f, 0.10f, 0.45f, false);

    mesh->CreateRenderData();
    g_lighthouseMeshCache[cacheKey] = meshEntity;

    weLog("    Created procedural lighthouse mesh h=" + std::to_string(totalHeight) +
          " baseR=" + std::to_string(baseR));

    wi::ecs::Entity entity = scene.Entity_CreateObject(name);
    auto* object = scene.objects.GetComponent(entity);
    if (object) object->meshID = meshEntity;
    return entity;
}

// Shared placeholder mesh cache -- avoids creating hundreds of identical GPU meshes
struct PlaceholderMeshKey {
    int ri, gi, bi, si; // color (0-255) and size (int mm)
    bool operator==(const PlaceholderMeshKey& o) const {
        return ri == o.ri && gi == o.gi && bi == o.bi && si == o.si;
    }
};
struct PlaceholderMeshKeyHash {
    size_t operator()(const PlaceholderMeshKey& k) const {
        return std::hash<int>()(k.ri * 1000000 + k.gi * 10000 + k.bi * 100 + k.si);
    }
};
static std::unordered_map<PlaceholderMeshKey, wi::ecs::Entity, PlaceholderMeshKeyHash> g_sharedPlaceholderMeshes;

// Get or create a shared placeholder box mesh+material (one GPU allocation per unique color+size)
static wi::ecs::Entity getSharedPlaceholderMesh(wi::scene::Scene& scene,
                                                  float r, float g, float b, float size) {
    PlaceholderMeshKey key = {(int)(r * 255), (int)(g * 255), (int)(b * 255), (int)(size * 1000)};
    auto it = g_sharedPlaceholderMeshes.find(key);
    if (it != g_sharedPlaceholderMeshes.end()) return it->second;

    // Create shared mesh + material (only done once per unique color+size)
    wi::ecs::Entity meshEntity = scene.Entity_CreateMesh("SharedPlaceholder_mesh");
    auto* mesh = scene.meshes.GetComponent(meshEntity);
    if (!mesh) return wi::ecs::INVALID_ENTITY;

    wi::ecs::Entity matEntity = scene.Entity_CreateMaterial("SharedPlaceholder_mat");
    auto* material = scene.materials.GetComponent(matEntity);
    if (material) {
        material->baseColor = DirectX::XMFLOAT4(r, g, b, 1.0f);
        material->CreateRenderData();
    }

    float h = size * 0.5f;
    DirectX::XMFLOAT3 verts[8] = {
        {-h, 0,  -h}, { h, 0,  -h}, { h, size, -h}, {-h, size, -h},
        {-h, 0,   h}, { h, 0,   h}, { h, size,  h}, {-h, size,  h}
    };
    uint32_t indices[36] = {
        0,2,1, 0,3,2, 4,5,6, 4,6,7,
        0,1,5, 0,5,4, 3,6,2, 3,7,6,
        0,4,7, 0,7,3, 1,2,6, 1,6,5
    };

    mesh->subsets.push_back(wi::scene::MeshComponent::MeshSubset());
    mesh->subsets.back().materialID = matEntity;
    mesh->subsets.back().indexOffset = 0;
    for (int i = 0; i < 8; i++) {
        mesh->vertex_positions.push_back(verts[i]);
        mesh->vertex_normals.push_back(DirectX::XMFLOAT3(0, 1, 0));
        mesh->vertex_uvset_0.push_back(DirectX::XMFLOAT2(0, 0));
    }
    for (int i = 0; i < 36; i++) mesh->indices.push_back(indices[i]);
    mesh->subsets.back().indexCount = 36;
    mesh->CreateRenderData();

    g_sharedPlaceholderMeshes[key] = meshEntity;
    weLog("    Created shared placeholder mesh (r=" + std::to_string(r) +
          " g=" + std::to_string(g) + " b=" + std::to_string(b) +
          " size=" + std::to_string(size) + ")");
    return meshEntity;
}

// Create a placeholder box instance that references a shared mesh (no new GPU allocation)
static wi::ecs::Entity createPlaceholderBox(wi::scene::Scene& scene, const std::string& name,
                                             float r, float g, float b, float size = 5.0f) {
    wi::ecs::Entity sharedMesh = getSharedPlaceholderMesh(scene, r, g, b, size);
    if (sharedMesh == wi::ecs::INVALID_ENTITY) return wi::ecs::INVALID_ENTITY;

    wi::ecs::Entity entity = scene.Entity_CreateObject(name);
    auto* object = scene.objects.GetComponent(entity);
    if (object) object->meshID = sharedMesh;
    return entity;
}

// ===== Navigation light lens flare: WE-native depth-tested screen-space glow =====
// Replaces emissive sphere markers. Flare visibility is automatically gated by
// depth buffer occlusion (hull blocks own-ship flares) and by setting intensity=0
// when the light should be invisible (out of arc, out of range, flash-off).
//
// Physics: at 1nm a 155mm lantern subtends 0.15 screen pixels (1080p/60deg FOV) --
// always sub-pixel. Real lights appear as pinpricks with glow from the eye's Airy
// disk and atmospheric scatter. We use a 4x4 pixel screen-space billboard for the
// pinprick (billboard size = texture_pixels / canvas_pixels), and rely on WE's bloom
// post-process to create apparent size proportional to brightness.
// NOTE: bridge window glass is alpha-blended and does NOT write to the depth buffer.
// Per-pixel depth testing in lensFlarePS.hlsl clips opaque geometry (hull, terrain)
// but cannot clip transparent window frames. The 4px billboard prevents frame bleed.
//
// Brightness follows Allard's Law: E = I * T^D / D^2 where T=0.8/nm (clear conditions).
// COLREG Annex I candela: 2nm sidelight=4.3cd, 3nm=12cd, 5nm masthead=52cd, 6nm=94cd.
static wi::Resource g_flareWhite, g_flareRed, g_flareGreen;

static void writeTGA(const std::string& path, int w, int h, const uint8_t* rgba) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return;
    uint8_t header[18] = {};
    header[2] = 2; // uncompressed true-color
    header[12] = (uint8_t)(w & 0xFF); header[13] = (uint8_t)((w >> 8) & 0xFF);
    header[14] = (uint8_t)(h & 0xFF); header[15] = (uint8_t)((h >> 8) & 0xFF);
    header[16] = 32; // 32-bit BGRA
    header[17] = 0x28; // top-left origin + 8 alpha bits
    f.write((const char*)header, 18);
    for (int i = 0; i < w * h; i++) {
        uint8_t bgra[4] = { rgba[i*4+2], rgba[i*4+1], rgba[i*4+0], rgba[i*4+3] };
        f.write((const char*)bgra, 4);
    }
}

// Estimate COLREG candela from nominal range using Allard's Law inverted:
// I = E_threshold * D_m^2 / T^D_nm
// E_threshold = 2e-7 lux (COLREG Annex I), T = 0.8/nm (10nm met. visibility)
// Results: 2nm->4.3cd, 3nm->12cd, 5nm->52cd (matches COLREG Table)
static float colregCandela(float rangeNM) {
    if (rangeNM <= 0) rangeNM = 2.0f;
    float D_m = rangeNM * 1852.0f;
    return 2.0e-7f * D_m * D_m / std::pow(0.8f, rangeNM);
}

static void generateNavLightFlareTextures() {
    // 4x4 pixel textures: tiny pinprick dot.  Billboard size on screen
    // equals texture_pixels (4 px across at 1080p).  Bridge window glass
    // is alpha-blended and does NOT write to the depth buffer, so the
    // per-pixel depth test in lensFlarePS.hlsl cannot clip billboard
    // pixels that overlap the transparent frame material.  Keeping the
    // billboard at 4 px prevents visible bleed-through entirely.
    // WE bloom (applied after lens flares in the post-process chain)
    // spreads the bright additive dot into a natural glow.
    const int SZ = 4;
    struct FlareSpec { const char* name; float r, g, b; };
    FlareSpec specs[] = {
        {"flare_nav_white.tga", 1.0f, 1.0f, 0.95f},   // bright white
        {"flare_nav_red.tga",   1.0f, 0.1f, 0.05f},    // COLREG red
        {"flare_nav_green.tga", 0.05f, 1.0f, 0.15f},   // COLREG green
    };

    std::vector<uint8_t> pixels(SZ * SZ * 4);
    for (auto& spec : specs) {
        for (int y = 0; y < SZ; y++) {
            for (int x = 0; x < SZ; x++) {
                int idx = (y * SZ + x) * 4;
                pixels[idx + 0] = (uint8_t)(spec.r * 255.0f);
                pixels[idx + 1] = (uint8_t)(spec.g * 255.0f);
                pixels[idx + 2] = (uint8_t)(spec.b * 255.0f);
                pixels[idx + 3] = 255; // fully opaque, no falloff
            }
        }
        writeTGA(spec.name, SZ, SZ, pixels.data());
    }
}

static void loadNavLightFlareTextures() {
    generateNavLightFlareTextures();
    g_flareWhite = wi::resourcemanager::Load("flare_nav_white.tga");
    g_flareRed   = wi::resourcemanager::Load("flare_nav_red.tga");
    g_flareGreen = wi::resourcemanager::Load("flare_nav_green.tga");
    int loaded = (g_flareWhite.IsValid() ? 1 : 0) +
                 (g_flareRed.IsValid() ? 1 : 0) +
                 (g_flareGreen.IsValid() ? 1 : 0);
    weLog("  Nav light flare textures: " + std::to_string(loaded) + "/3 loaded");
}

static void attachLensFlare(wi::scene::Scene& scene, wi::ecs::Entity lightEntity,
                             float r, float g, float b) {
    // Pick colored flare matching light color (COLREG: red port, green stbd, white mast/stern)
    wi::Resource* flare;
    if (r > 0.5f && g < 0.3f && b < 0.3f)      flare = &g_flareRed;
    else if (g > 0.5f && r < 0.3f && b < 0.3f)  flare = &g_flareGreen;
    else                                          flare = &g_flareWhite;

    if (!flare->IsValid()) {
        weLog("  [FLARE] texture not valid for entity " + std::to_string(lightEntity));
        return;
    }
    auto* lc = scene.lights.GetComponent(lightEntity);
    if (!lc) {
        weLog("  [FLARE] no LightComponent for entity " + std::to_string(lightEntity));
        return;
    }
    lc->lensFlareRimTextures.push_back(*flare);
    weLog("  [FLARE] attached to entity " + std::to_string(lightEntity) +
          " flareCount=" + std::to_string(lc->lensFlareRimTextures.size()));
}

// Cache of WE mesh entities created from Irrlicht-converted models (by filepath)
static std::unordered_map<std::string, wi::ecs::Entity> g_convertedMeshCache;

// Create a WE mesh entity from Irrlicht-converted model data (standalone, not attached to any root)
// modelDir: directory containing the model (for resolving relative texture paths)
static wi::ecs::Entity createWEMeshFromConverted(wi::scene::Scene& scene,
                                                   const bc::ConvertedModel& model,
                                                   const std::string& baseName,
                                                   const std::string& modelDir = "",
                                                   bool allowTransparency = false) {
    wi::ecs::Entity meshEntity = scene.Entity_CreateMesh(baseName + "_mesh");
    auto* mesh = scene.meshes.GetComponent(meshEntity);
    if (!mesh) return wi::ecs::INVALID_ENTITY;

    uint32_t vertexOffset = 0;
    for (size_t i = 0; i < model.submeshes.size(); i++) {
        const auto& sub = model.submeshes[i];

        // Create material (standalone entity)
        wi::ecs::Entity matEntity = scene.Entity_CreateMaterial(
            baseName + "_mat" + std::to_string(i));
        auto* material = scene.materials.GetComponent(matEntity);
        if (material) {
            // Determine effective alpha:
            // - Buildings/buoys: force opaque (unreliable alpha from .3ds/.x formats)
            // - Ships: allow transparency for bridge windows (alpha 0.01-0.95)
            float effectiveAlpha = 1.0f;
            if (allowTransparency && sub.material.a > 0.01f && sub.material.a < 0.95f) {
                // Genuinely semi-transparent (e.g. ship bridge windows)
                effectiveAlpha = sub.material.a * 0.05f; // near-invisible glass
            }
            material->baseColor = DirectX::XMFLOAT4(
                sub.material.r, sub.material.g, sub.material.b, effectiveAlpha);
            material->emissiveColor = DirectX::XMFLOAT4(
                sub.material.er, sub.material.eg, sub.material.eb,
                std::max({sub.material.er, sub.material.eg, sub.material.eb}));
            float roughness = 1.0f - (sub.material.shininess / 128.0f);
            material->roughness = std::max(0.04f, std::min(1.0f, roughness));
            material->metalness = 0.0f; // buildings/structures are non-metallic

            // Color-only models (no texture): use high roughness to prevent
            // sky/environment reflections from tinting surfaces blue in PBR
            if (sub.material.textureName.empty() && roughness > 0.7f) {
                material->roughness = 0.85f;
            }

            // Assign texture from Irrlicht material data.
            // EDT_NULL creates SDummyTexture objects for materials that reference
            // texture files, so textureName is correct for textured materials.
            // Materials without textureName are genuinely untextured (color-only).
            std::string texPath;
            if (!sub.material.textureName.empty()) {
                texPath = sub.material.textureName;
                // If just a filename (no directory), prepend model directory
                if (!modelDir.empty() && texPath.find('/') == std::string::npos &&
                    texPath.find('\\') == std::string::npos &&
                    texPath.find(':') == std::string::npos) {
                    texPath = modelDir + texPath;
                }
            }
            if (!texPath.empty()) {
                // Normalize backslashes to forward slashes (WE convention)
                std::replace(texPath.begin(), texPath.end(), '\\', '/');
                // Convert relative paths to absolute (WE resolves from internal root)
                if (texPath.find(':') == std::string::npos && !texPath.empty()) {
                    texPath = wi::helper::GetCurrentPath() + "/" + texPath;
                }
                // Runtime texture upscaling: bilinear 2x for textures below 512px
                {
                    std::string upscaled = TextureUpscaler::ensureMinSize(
                        texPath, Utilities::getUserDirBase() + "texcache/", 512);
                    if (!upscaled.empty()) {
                        weLog("    Upscaled: " + texPath + " -> " + upscaled);
                        texPath = upscaled;
                    }
                }
                material->textures[wi::scene::MaterialComponent::BASECOLORMAP].name = texPath;
                // Explicitly pre-load texture into resource manager before CreateRenderData
                // (CreateRenderData queues async load but may not resolve without this)
                if (wi::helper::FileExists(texPath)) {
                    material->textures[wi::scene::MaterialComponent::BASECOLORMAP].resource =
                        wi::resourcemanager::Load(texPath);
                    // In Irrlicht, DiffuseColor affects lighting, not texture tinting.
                    // In WE PBR, baseColor * texture = final albedo. Set white to avoid
                    // DiffuseColor tinting the texture (e.g. blue console panels).
                    material->baseColor = DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, effectiveAlpha);
                    weLog("    Texture[" + std::to_string(i) + "]: " + texPath + " [OK]");

                    // Auto-detect PBR maps alongside base texture
                    // Convention: wall.png -> wall_Normal.png, wall_Roughness.png
                    size_t dotPos = texPath.rfind('.');
                    if (dotPos != std::string::npos) {
                        std::string stem = texPath.substr(0, dotPos);
                        std::string ext = texPath.substr(dotPos);

                        std::string normalPath = stem + "_Normal" + ext;
                        if (wi::helper::FileExists(normalPath)) {
                            material->textures[wi::scene::MaterialComponent::NORMALMAP].name = normalPath;
                            material->textures[wi::scene::MaterialComponent::NORMALMAP].resource =
                                wi::resourcemanager::Load(normalPath);
                            weLog("    Normal[" + std::to_string(i) + "]: " + normalPath + " [OK]");
                        }

                        // WE SURFACEMAP = packed: R=occlusion, G=roughness, B=metalness, A=unused
                        // If we find a _Roughness map, load it as surfacemap
                        std::string roughPath = stem + "_Roughness" + ext;
                        if (wi::helper::FileExists(roughPath)) {
                            material->textures[wi::scene::MaterialComponent::SURFACEMAP].name = roughPath;
                            material->textures[wi::scene::MaterialComponent::SURFACEMAP].resource =
                                wi::resourcemanager::Load(roughPath);
                            weLog("    Surface[" + std::to_string(i) + "]: " + roughPath + " [OK]");
                        }
                    }
                } else {
                    weLog("    Texture[" + std::to_string(i) + "]: " + texPath + " [MISSING]");
                    weLog("      Raw Irrlicht name: " + sub.material.textureName);
                }
            } else {
                // No texture for this submesh - log its diffuse color for debugging
                weLog("    Submesh[" + std::to_string(i) + "]: no texture, diffuse=(" +
                      std::to_string(sub.material.r) + "," + std::to_string(sub.material.g) +
                      "," + std::to_string(sub.material.b) + "," + std::to_string(sub.material.a) + ")");
            }

            // Models from mixed formats (.x=CW, .3ds=CCW winding), render both sides
            material->SetDoubleSided(true);

            // Enable alpha blending for ship windows
            if (effectiveAlpha < 0.95f) {
                material->userBlendMode = wi::enums::BLENDMODE_ALPHA;
                material->SetCastShadow(false);
            }

            material->CreateRenderData();
        }

        // Add mesh subset
        mesh->subsets.push_back(wi::scene::MeshComponent::MeshSubset());
        auto& subset = mesh->subsets.back();
        subset.materialID = matEntity;
        subset.indexOffset = static_cast<uint32_t>(mesh->indices.size());
        subset.indexCount = static_cast<uint32_t>(sub.indices.size());

        // Add vertices (no Z-flip: .x files are already left-handed like WE)
        // Sanitize UVs: clamp garbage values (e.g. 4.9e19 from corrupt .x data)
        for (const auto& v : sub.vertices) {
            mesh->vertex_positions.push_back(DirectX::XMFLOAT3(v.px, v.py, v.pz));
            mesh->vertex_normals.push_back(DirectX::XMFLOAT3(v.nx, v.ny, v.nz));
            float su = v.u, sv = v.v;
            if (std::abs(su) > 1e6f || std::isnan(su) || std::isinf(su)) su = 0.0f;
            if (std::abs(sv) > 1e6f || std::isnan(sv) || std::isinf(sv)) sv = 0.0f;
            mesh->vertex_uvset_0.push_back(DirectX::XMFLOAT2(su, sv));
        }

        // Add indices (offset by vertices from previous submeshes)
        for (uint32_t idx : sub.indices) {
            mesh->indices.push_back(idx + vertexOffset);
        }

        vertexOffset += static_cast<uint32_t>(sub.vertices.size());
    }

    mesh->CreateRenderData();
    return meshEntity;
}

// Create an object entity referencing a shared mesh
static wi::ecs::Entity createObjectFromMesh(wi::scene::Scene& scene,
                                              wi::ecs::Entity meshEntity,
                                              const std::string& name) {
    wi::ecs::Entity rootEntity = wi::ecs::CreateEntity();
    scene.transforms.Create(rootEntity);
    scene.names.Create(rootEntity) = name;

    wi::ecs::Entity objectEntity = scene.Entity_CreateObject(name + "_obj");
    scene.Component_Attach(objectEntity, rootEntity);
    auto* object = scene.objects.GetComponent(objectEntity);
    if (object) object->meshID = meshEntity;
    return rootEntity;
}

// Try to load a model: WE native → Irrlicht conversion → placeholder box
static wi::ecs::Entity loadModelOrPlaceholder(wi::scene::Scene& scene,
                                               const std::string& modelPath,
                                               const std::string& name,
                                               float r, float g, float b,
                                               float placeholderSize = 5.0f,
                                               bool allowTransparency = false) {
    // Fast path: reuse already-converted mesh
    auto cachedIt = g_convertedMeshCache.find(modelPath);
    if (cachedIt != g_convertedMeshCache.end()) {
        return createObjectFromMesh(scene, cachedIt->second, name);
    }

    // Try WE native loader first (.obj, .gltf, .wiscene)
    wi::ecs::Entity entity = wi::ecs::INVALID_ENTITY;
    try {
        entity = bc::graphics::wicked::LoadModelFromFile(modelPath, scene);
    } catch (...) {
        weLogErr("Exception in LoadModelFromFile: " + modelPath);
    }
    if (entity != wi::ecs::INVALID_ENTITY) return entity;

    // Try Irrlicht conversion (.x, .3ds, etc.)
    try {
        bc::ConvertedModel converted = bc::convertModelViaIrrlicht(modelPath);
        if (converted.valid) {
            size_t totalVerts = 0, totalIdx = 0;
            for (const auto& s : converted.submeshes) {
                totalVerts += s.vertices.size();
                totalIdx += s.indices.size();
            }
            // Get model directory for resolving relative texture paths
            std::string modelDir;
            size_t lastSlash = modelPath.find_last_of("/\\");
            if (lastSlash != std::string::npos)
                modelDir = modelPath.substr(0, lastSlash + 1);

            // Count submeshes with texture names from Irrlicht
            size_t irrlichtTexCount = 0;
            for (const auto& s : converted.submeshes) {
                if (!s.material.textureName.empty()) irrlichtTexCount++;
            }

            wi::ecs::Entity meshEntity = createWEMeshFromConverted(
                scene, converted, name, modelDir, allowTransparency);
            if (meshEntity != wi::ecs::INVALID_ENTITY) {
                g_convertedMeshCache[modelPath] = meshEntity;
                weLog("    Loaded via Irrlicht: " + modelPath +
                      " (" + std::to_string(converted.submeshes.size()) + " submeshes, " +
                      std::to_string(totalVerts) + " verts, " +
                      std::to_string(totalIdx / 3) + " tris, " +
                      std::to_string(irrlichtTexCount) + "/" +
                      std::to_string(converted.submeshes.size()) + " textured)");
                return createObjectFromMesh(scene, meshEntity, name);
            }
        }
    } catch (...) {
        weLogErr("Exception in Irrlicht conversion: " + modelPath);
    }

    // Fall back to placeholder box
    weLog("    Using placeholder box for: " + modelPath);
    return createPlaceholderBox(scene, name, r, g, b, placeholderSize);
}

// ===== Buoy colour helpers =====

struct ColourBand {
    float r, g, b;
};

static void colourNameToRGB(const std::string& name, float& r, float& g, float& b) {
    // Case-insensitive matching
    std::string lower = name;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower == "red")    { r=0.80f; g=0.05f; b=0.05f; return; }
    if (lower == "green")  { r=0.05f; g=0.60f; b=0.05f; return; }
    if (lower == "yellow") { r=0.95f; g=0.85f; b=0.00f; return; }
    if (lower == "black")  { r=0.08f; g=0.08f; b=0.08f; return; }
    if (lower == "white")  { r=0.95f; g=0.95f; b=0.95f; return; }
    if (lower == "orange") { r=0.95f; g=0.50f; b=0.00f; return; }
    if (lower == "blue")   { r=0.00f; g=0.20f; b=0.80f; return; }
    r=0.95f; g=0.95f; b=0.95f; // default white
}

static std::vector<ColourBand> parseColourBands(const std::string& colours) {
    std::vector<ColourBand> bands;
    std::istringstream iss(colours);
    std::string token;
    while (std::getline(iss, token, ';')) {
        // Trim whitespace
        while (!token.empty() && token.front() == ' ') token.erase(token.begin());
        while (!token.empty() && token.back() == ' ') token.pop_back();
        if (token.empty()) continue;
        ColourBand band;
        colourNameToRGB(token, band.r, band.g, band.b);
        bands.push_back(band);
    }
    return bands;
}

// Infer IALA cardinal topmark type from colour pattern.
// Returns: 0=none, 1=North (both up), 2=South (both down),
//          3=East (base-to-base), 4=West (point-to-point)
static int inferCardinalTopmark(const std::string& colours) {
    if (colours == "black;yellow") return 1;
    if (colours == "yellow;black") return 2;
    if (colours == "black;yellow;black") return 3;
    if (colours == "yellow;black;yellow") return 4;
    return 0;
}

// Helper: add a cone to the mesh (black material). yBase = wide end, yApex = point end.
static void addConeToMesh(wi::scene::MeshComponent* mesh, wi::scene::Scene& scene,
                           const std::string& matName,
                           float yBase, float yApex, float baseRadius, int segments) {
    wi::ecs::Entity matEntity = scene.Entity_CreateMaterial(matName);
    auto* mat = scene.materials.GetComponent(matEntity);
    if (mat) {
        mat->baseColor = DirectX::XMFLOAT4(0.08f, 0.08f, 0.08f, 1.0f); // black
        mat->roughness = 0.40f;
        mat->metalness = 0.0f;
        mat->SetDoubleSided(true);
        mat->CreateRenderData();
    }

    float ny = (yApex > yBase) ? 0.5f : -0.5f; // normal Y hint for slant

    // Apex vertex
    uint32_t apexIdx = (uint32_t)mesh->vertex_positions.size();
    mesh->vertex_positions.push_back({0.0f, yApex, 0.0f});
    mesh->vertex_normals.push_back({0.0f, ny, 0.0f});
    mesh->vertex_uvset_0.push_back({0.5f, 0.5f});

    // Base ring
    uint32_t ringBase = (uint32_t)mesh->vertex_positions.size();
    for (int i = 0; i <= segments; i++) {
        float a = (float)i / segments * 6.28318530718f;
        float ca = cosf(a), sa = sinf(a);
        mesh->vertex_positions.push_back({baseRadius * ca, yBase, baseRadius * sa});
        mesh->vertex_normals.push_back({ca * 0.866f, ny * 0.5f, sa * 0.866f}); // slanted normal
        mesh->vertex_uvset_0.push_back({(float)i / segments, 0.0f});
    }

    // Cone side triangles
    wi::scene::MeshComponent::MeshSubset subset;
    subset.materialID = matEntity;
    subset.indexOffset = (uint32_t)mesh->indices.size();
    for (int i = 0; i < segments; i++) {
        if (yApex > yBase) {
            // Point up: apex, ring[i], ring[i+1]
            mesh->indices.push_back(apexIdx);
            mesh->indices.push_back(ringBase + i);
            mesh->indices.push_back(ringBase + i + 1);
        } else {
            // Point down: apex, ring[i+1], ring[i] (reversed winding)
            mesh->indices.push_back(apexIdx);
            mesh->indices.push_back(ringBase + i + 1);
            mesh->indices.push_back(ringBase + i);
        }
    }
    subset.indexCount = (uint32_t)(mesh->indices.size() - subset.indexOffset);
    mesh->subsets.push_back(subset);
}

// Create a procedural cylinder with horizontal colour bands for multi-band buoys.
// Bands are ordered top-to-bottom per IALA/OSM: "black;yellow" = black on top.
// topmarkType: 0=none, 1=N(both up), 2=S(both down), 3=E(base-to-base), 4=W(point-to-point)
static wi::ecs::Entity createBandedBuoyCylinder(wi::scene::Scene& scene,
                                                  const std::string& name,
                                                  const std::vector<ColourBand>& bands,
                                                  float height, float radius,
                                                  int topmarkType = 0) {
    const int N = 12; // circumference segments

    wi::ecs::Entity meshEntity = scene.Entity_CreateMesh(name + "_bandmesh");
    auto* mesh = scene.meshes.GetComponent(meshEntity);
    if (!mesh) return wi::ecs::INVALID_ENTITY;

    float bandH = height / static_cast<float>(bands.size());

    // Bands[0] = top, bands[N-1] = bottom per IALA convention
    for (size_t b = 0; b < bands.size(); b++) {
        float yTop = height - b * bandH;
        float yBot = height - (b + 1) * bandH;

        wi::ecs::Entity matEntity = scene.Entity_CreateMaterial(name + "_bmat" + std::to_string(b));
        auto* mat = scene.materials.GetComponent(matEntity);
        if (mat) {
            mat->baseColor = DirectX::XMFLOAT4(bands[b].r, bands[b].g, bands[b].b, 1.0f);
            mat->roughness = 0.45f;
            mat->metalness = 0.0f;
            mat->SetDoubleSided(true);
            mat->CreateRenderData();
        }

        uint32_t base = (uint32_t)mesh->vertex_positions.size();
        for (int i = 0; i <= N; i++) {
            float a = (float)i / N * 6.28318530718f;
            float ca = cosf(a), sa = sinf(a);
            mesh->vertex_positions.push_back({radius * ca, yBot, radius * sa});
            mesh->vertex_normals.push_back({ca, 0.0f, sa});
            mesh->vertex_uvset_0.push_back({(float)i / N, 0.0f});
            mesh->vertex_positions.push_back({radius * ca, yTop, radius * sa});
            mesh->vertex_normals.push_back({ca, 0.0f, sa});
            mesh->vertex_uvset_0.push_back({(float)i / N, 1.0f});
        }

        wi::scene::MeshComponent::MeshSubset subset;
        subset.materialID = matEntity;
        subset.indexOffset = (uint32_t)mesh->indices.size();
        for (int i = 0; i < N; i++) {
            uint32_t bl = base + i * 2, tl = bl + 1;
            uint32_t br = base + (i + 1) * 2, tr = br + 1;
            mesh->indices.push_back(bl); mesh->indices.push_back(tl); mesh->indices.push_back(br);
            mesh->indices.push_back(br); mesh->indices.push_back(tl); mesh->indices.push_back(tr);
        }
        subset.indexCount = (uint32_t)(mesh->indices.size() - subset.indexOffset);
        mesh->subsets.push_back(subset);
    }

    // Top cap (uses top band colour)
    {
        uint32_t base = (uint32_t)mesh->vertex_positions.size();
        wi::ecs::Entity capMat = scene.Entity_CreateMaterial(name + "_capm");
        auto* cm = scene.materials.GetComponent(capMat);
        if (cm) {
            cm->baseColor = DirectX::XMFLOAT4(bands[0].r, bands[0].g, bands[0].b, 1.0f);
            cm->roughness = 0.45f; cm->metalness = 0.0f;
            cm->SetDoubleSided(true); cm->CreateRenderData();
        }
        mesh->vertex_positions.push_back({0.0f, height, 0.0f});
        mesh->vertex_normals.push_back({0.0f, 1.0f, 0.0f});
        mesh->vertex_uvset_0.push_back({0.5f, 0.5f});
        for (int i = 0; i <= N; i++) {
            float a = (float)i / N * 6.28318530718f;
            mesh->vertex_positions.push_back({radius * cosf(a), height, radius * sinf(a)});
            mesh->vertex_normals.push_back({0.0f, 1.0f, 0.0f});
            mesh->vertex_uvset_0.push_back({0.5f + 0.5f * cosf(a), 0.5f + 0.5f * sinf(a)});
        }
        wi::scene::MeshComponent::MeshSubset capSub;
        capSub.materialID = capMat;
        capSub.indexOffset = (uint32_t)mesh->indices.size();
        for (int i = 0; i < N; i++) {
            mesh->indices.push_back(base);
            mesh->indices.push_back(base + 1 + i);
            mesh->indices.push_back(base + 1 + i + 1);
        }
        capSub.indexCount = (uint32_t)(mesh->indices.size() - capSub.indexOffset);
        mesh->subsets.push_back(capSub);
    }

    // IALA cardinal topmarks: two black cones on a staff above the body
    if (topmarkType >= 1 && topmarkType <= 4) {
        float staffR = radius * 0.08f;  // thin staff
        float coneR = radius * 0.55f;   // cone base radius
        float coneH = height * 0.18f;   // cone height
        float gap = height * 0.05f;     // gap between cones

        // Staff: thin cylinder from body top to above topmarks
        float staffBot = height;
        float staffTop = height + coneH * 2.0f + gap * 3.0f;
        {
            wi::ecs::Entity sMat = scene.Entity_CreateMaterial(name + "_staff");
            auto* sm = scene.materials.GetComponent(sMat);
            if (sm) {
                sm->baseColor = DirectX::XMFLOAT4(0.08f, 0.08f, 0.08f, 1.0f);
                sm->roughness = 0.40f; sm->metalness = 0.0f;
                sm->SetDoubleSided(true); sm->CreateRenderData();
            }
            uint32_t base = (uint32_t)mesh->vertex_positions.size();
            for (int i = 0; i <= N; i++) {
                float a = (float)i / N * 6.28318530718f;
                float ca = cosf(a), sa = sinf(a);
                mesh->vertex_positions.push_back({staffR * ca, staffBot, staffR * sa});
                mesh->vertex_normals.push_back({ca, 0, sa});
                mesh->vertex_uvset_0.push_back({(float)i / N, 0});
                mesh->vertex_positions.push_back({staffR * ca, staffTop, staffR * sa});
                mesh->vertex_normals.push_back({ca, 0, sa});
                mesh->vertex_uvset_0.push_back({(float)i / N, 1});
            }
            wi::scene::MeshComponent::MeshSubset sSub;
            sSub.materialID = sMat;
            sSub.indexOffset = (uint32_t)mesh->indices.size();
            for (int i = 0; i < N; i++) {
                uint32_t bl = base + i * 2, tl = bl + 1;
                uint32_t br = base + (i + 1) * 2, tr = br + 1;
                mesh->indices.push_back(bl); mesh->indices.push_back(tl); mesh->indices.push_back(br);
                mesh->indices.push_back(br); mesh->indices.push_back(tl); mesh->indices.push_back(tr);
            }
            sSub.indexCount = (uint32_t)(mesh->indices.size() - sSub.indexOffset);
            mesh->subsets.push_back(sSub);
        }

        // Position cones based on cardinal direction
        float y1Base, y1Apex, y2Base, y2Apex; // cone 1 (lower), cone 2 (upper)
        float yOff = height + gap; // base offset above body

        switch (topmarkType) {
        case 1: // North: both cones point UP
            y1Base = yOff;
            y1Apex = yOff + coneH;
            y2Base = yOff + coneH + gap;
            y2Apex = yOff + coneH * 2.0f + gap;
            break;
        case 2: // South: both cones point DOWN
            y1Apex = yOff;
            y1Base = yOff + coneH;
            y2Apex = yOff + coneH + gap;
            y2Base = yOff + coneH * 2.0f + gap;
            break;
        case 3: // East: base-to-base (lower points down, upper points up)
            y1Apex = yOff;
            y1Base = yOff + coneH;
            y2Base = yOff + coneH; // bases touch
            y2Apex = yOff + coneH * 2.0f;
            break;
        case 4: // West: point-to-point (lower points up, upper points down)
            y1Base = yOff;
            y1Apex = yOff + coneH;
            y2Apex = yOff + coneH; // points touch
            y2Base = yOff + coneH * 2.0f;
            break;
        default:
            y1Base = y1Apex = y2Base = y2Apex = 0;
            break;
        }

        addConeToMesh(mesh, scene, name + "_cone1", y1Base, y1Apex, coneR, N);
        addConeToMesh(mesh, scene, name + "_cone2", y2Base, y2Apex, coneR, N);
    }

    mesh->CreateRenderData();
    return meshEntity;
}

// Load a buoy with colour override. For multi-band buoys (cardinals, isolated danger),
// uses procedural cylinder geometry to guarantee clean equal-height bands.
// For single-colour buoys, applies colour to the model's existing geometry.
static wi::ecs::Entity loadBuoyWithColour(wi::scene::Scene& scene,
                                            const std::string& modelPath,
                                            const std::string& name,
                                            const std::string& colours) {
    std::string cacheKey = modelPath + "|" + colours;
    auto cachedIt = g_convertedMeshCache.find(cacheKey);
    if (cachedIt != g_convertedMeshCache.end()) {
        return createObjectFromMesh(scene, cachedIt->second, name);
    }

    std::vector<ColourBand> bands = parseColourBands(colours);
    if (bands.empty()) {
        return loadModelOrPlaceholder(scene, modelPath, name, 0.8f, 0.2f, 0.2f, 3.0f);
    }

    // Multi-band buoys: use procedural cylinder for guaranteed correct IALA bands.
    // Model geometry often has uneven vertex distribution (wide base, narrow top)
    // which makes per-triangle banding look wrong.
    if (bands.size() > 1) {
        // Height 10 units (scaled to world size by ScaleFactor from buoy.ini)
        int topmark = inferCardinalTopmark(colours);
        wi::ecs::Entity meshEntity = createBandedBuoyCylinder(scene, name, bands, 10.0f, 1.5f, topmark);
        g_convertedMeshCache[cacheKey] = meshEntity;
        weLog("    Created banded buoy cylinder: " + name + " colours=" + colours +
              " (" + std::to_string(bands.size()) + " bands" +
              (topmark > 0 ? ", topmark=" + std::to_string(topmark) : "") + ")");
        return createObjectFromMesh(scene, meshEntity, name);
    }

    // Single-colour buoys: load model geometry and apply colour
    bc::ConvertedModel converted;
    try {
        converted = bc::convertModelViaIrrlicht(modelPath);
    } catch (...) {
        weLogErr("Exception converting buoy: " + modelPath);
    }

    if (!converted.valid || converted.submeshes.empty()) {
        weLog("    Using coloured placeholder for: " + modelPath + " colours=" + colours);
        return createPlaceholderBox(scene, name, bands[0].r, bands[0].g, bands[0].b, 3.0f);
    }

    // Apply single colour to all submeshes
    wi::ecs::Entity meshEntity = scene.Entity_CreateMesh(name + "_colmesh");
    auto* mesh = scene.meshes.GetComponent(meshEntity);
    if (!mesh) {
        return createPlaceholderBox(scene, name, bands[0].r, bands[0].g, bands[0].b, 3.0f);
    }

    wi::ecs::Entity matEntity = scene.Entity_CreateMaterial(name + "_colmat");
    auto* material = scene.materials.GetComponent(matEntity);
    if (material) {
        material->baseColor = DirectX::XMFLOAT4(bands[0].r, bands[0].g, bands[0].b, 1.0f);
        material->roughness = 0.45f;
        material->metalness = 0.0f;
        material->SetDoubleSided(true);
        material->CreateRenderData();
    }

    uint32_t vertOff = 0;
    for (const auto& sub : converted.submeshes) {
        for (const auto& v : sub.vertices) {
            mesh->vertex_positions.push_back({v.px, v.py, v.pz});
            mesh->vertex_normals.push_back({v.nx, v.ny, v.nz});
            mesh->vertex_uvset_0.push_back({v.u, v.v});
        }
        for (uint32_t idx : sub.indices) {
            mesh->indices.push_back(idx + vertOff);
        }
        vertOff += (uint32_t)sub.vertices.size();
    }

    mesh->subsets.push_back(wi::scene::MeshComponent::MeshSubset());
    auto& subset = mesh->subsets.back();
    subset.materialID = matEntity;
    subset.indexOffset = 0;
    subset.indexCount = (uint32_t)mesh->indices.size();

    mesh->CreateRenderData();
    g_convertedMeshCache[cacheKey] = meshEntity;
    weLog("    Loaded buoy with colour: " + modelPath + " colour=" + colours);
    return createObjectFromMesh(scene, meshEntity, name);
}

// Create a WE mesh entity from a BuildingMesh (procedural geometry, no model file)
// Supports separate wall/roof materials via wallIndexCount split.
// isStructure: if true, uses concrete material instead of facade textures.
static wi::ecs::Entity createBuildingMeshEntity(wi::scene::Scene& scene,
                                                  const BuildingMesh& bm,
                                                  const std::string& name,
                                                  const std::string& wallTexturePath = "",
                                                  const std::string& roofTexturePath = "",
                                                  bool isStructure = false) {
    if (bm.empty()) return wi::ecs::INVALID_ENTITY;

    wi::ecs::Entity meshEntity = scene.Entity_CreateMesh(name + "_mesh");
    auto* mesh = scene.meshes.GetComponent(meshEntity);
    if (!mesh) return wi::ecs::INVALID_ENTITY;

    uint32_t wallIdxCount = static_cast<uint32_t>(std::min(bm.wallIndexCount, bm.indices.size()));
    uint32_t roofIdxCount = static_cast<uint32_t>(bm.indices.size()) - wallIdxCount;

    if (isStructure) {
        // Harbour structures: concrete material (grey, rough, no texture)
        wi::ecs::Entity matEntity = scene.Entity_CreateMaterial(name + "_concrete");
        auto* material = scene.materials.GetComponent(matEntity);
        if (material) {
            material->baseColor = DirectX::XMFLOAT4(0.7f, 0.7f, 0.68f, 1.0f); // Warm grey concrete
            material->roughness = 0.85f;
            material->metalness = 0.0f;
            material->SetDoubleSided(true);
            material->SetCastShadow(true);
            material->CreateRenderData();
        }

        mesh->subsets.push_back(wi::scene::MeshComponent::MeshSubset());
        auto& subset = mesh->subsets.back();
        subset.materialID = matEntity;
        subset.indexOffset = 0;
        subset.indexCount = static_cast<uint32_t>(bm.indices.size());
    } else {
        // Buildings: separate wall and roof materials
        // Wall material
        wi::ecs::Entity wallMatEntity = scene.Entity_CreateMaterial(name + "_wall");
        auto* wallMat = scene.materials.GetComponent(wallMatEntity);
        if (wallMat) {
            wallMat->baseColor = DirectX::XMFLOAT4(0.9f, 0.9f, 0.9f, 1.0f);
            wallMat->roughness = 0.75f;
            wallMat->metalness = 0.0f;
            wallMat->SetDoubleSided(true);
            wallMat->SetCastShadow(true);
            if (!wallTexturePath.empty()) {
                wallMat->textures[wi::scene::MaterialComponent::BASECOLORMAP].name = wallTexturePath;
            }
            wallMat->CreateRenderData();
        }

        // Roof material (darker, rougher)
        wi::ecs::Entity roofMatEntity = scene.Entity_CreateMaterial(name + "_roof");
        auto* roofMat = scene.materials.GetComponent(roofMatEntity);
        if (roofMat) {
            roofMat->baseColor = DirectX::XMFLOAT4(0.6f, 0.6f, 0.6f, 1.0f);
            roofMat->roughness = 0.85f;
            roofMat->metalness = 0.0f;
            roofMat->SetDoubleSided(true);
            roofMat->SetCastShadow(true);
            if (!roofTexturePath.empty()) {
                roofMat->textures[wi::scene::MaterialComponent::BASECOLORMAP].name = roofTexturePath;
            }
            roofMat->CreateRenderData();
        }

        // Wall subset
        if (wallIdxCount > 0) {
            mesh->subsets.push_back(wi::scene::MeshComponent::MeshSubset());
            auto& wallSubset = mesh->subsets.back();
            wallSubset.materialID = wallMatEntity;
            wallSubset.indexOffset = 0;
            wallSubset.indexCount = wallIdxCount;
        }

        // Roof subset
        if (roofIdxCount > 0) {
            mesh->subsets.push_back(wi::scene::MeshComponent::MeshSubset());
            auto& roofSubset = mesh->subsets.back();
            roofSubset.materialID = roofMatEntity;
            roofSubset.indexOffset = wallIdxCount;
            roofSubset.indexCount = roofIdxCount;
        }
    }

    size_t nv = bm.vertexCount();
    for (size_t i = 0; i < nv; i++) {
        mesh->vertex_positions.push_back(DirectX::XMFLOAT3(
            bm.positions[i * 3], bm.positions[i * 3 + 1], bm.positions[i * 3 + 2]));
        mesh->vertex_normals.push_back(DirectX::XMFLOAT3(
            bm.normals[i * 3], bm.normals[i * 3 + 1], bm.normals[i * 3 + 2]));
        mesh->vertex_uvset_0.push_back(DirectX::XMFLOAT2(
            bm.uvs[i * 2], bm.uvs[i * 2 + 1]));
    }
    for (uint32_t idx : bm.indices) {
        mesh->indices.push_back(idx);
    }

    mesh->CreateRenderData();

    // Create object referencing this mesh
    wi::ecs::Entity rootEntity = wi::ecs::CreateEntity();
    scene.transforms.Create(rootEntity);
    scene.names.Create(rootEntity) = name;
    wi::ecs::Entity objectEntity = scene.Entity_CreateObject(name + "_obj");
    scene.Component_Attach(objectEntity, rootEntity);
    auto* object = scene.objects.GetComponent(objectEntity);
    if (object) object->meshID = meshEntity;

    return rootEntity;
}

// Toggle renderable on an entity and all its child objects
static void setEntityVisible(wi::scene::Scene& scene, wi::ecs::Entity entity, bool visible) {
    auto* obj = scene.objects.GetComponent(entity);
    if (obj) obj->SetRenderable(visible);
    for (size_t i = 0; i < scene.hierarchy.GetCount(); i++) {
        if (scene.hierarchy[i].parentID == entity) {
            wi::ecs::Entity child = scene.hierarchy.GetEntity(i);
            auto* childObj = scene.objects.GetComponent(child);
            if (childObj) childObj->SetRenderable(visible);
        }
    }
}

// Position and rotate a WE entity
static void setEntityTransform(wi::scene::Scene& scene, wi::ecs::Entity entity,
                                float x, float y, float z,
                                float rotYDeg = 0, float scale = 1.0f,
                                float pitchDeg = 0, float rollDeg = 0) {
    auto* transform = scene.transforms.GetComponent(entity);
    if (!transform) return;
    transform->ClearTransform();
    transform->Translate(DirectX::XMFLOAT3(x, y, z)); // both BC and WE are left-handed Y-up, no Z flip
    if (rotYDeg != 0 || pitchDeg != 0 || rollDeg != 0) {
        float yawRad = rotYDeg * (float)M_PI / 180.0f;
        float pitchRad = pitchDeg * (float)M_PI / 180.0f;
        float rollRad = rollDeg * (float)M_PI / 180.0f;
        // WE RotateRollPitchYaw: XMFLOAT3(pitch, yaw, roll)
        transform->RotateRollPitchYaw(DirectX::XMFLOAT3(pitchRad, yawRad, rollRad));
    }
    if (scale != 1.0f) {
        transform->Scale(DirectX::XMFLOAT3(scale, scale, scale));
    }
    transform->UpdateTransform();
}

// Resolve a model path, checking user folder and world folder for overrides
static std::string resolveModelPath(const std::string& basePath,
                                     const std::string& userFolder,
                                     const std::string& worldPath) {
    if (Utilities::pathExists(userFolder + basePath))
        return userFolder + basePath;
    if (!worldPath.empty() && Utilities::pathExists(worldPath + "/" + basePath))
        return worldPath + "/" + basePath;
    return basePath;
}

static int g_frameCountForCrash = 0; // accessible from crash handler

static LONG WINAPI weCrashHandler(EXCEPTION_POINTERS* ep) {
    if (g_weLog.is_open()) {
        DWORD code = ep->ExceptionRecord->ExceptionCode;
        void* addr = ep->ExceptionRecord->ExceptionAddress;
        g_weLog << "CRASH: exception 0x" << std::hex << code
                << " at address 0x" << addr << std::dec
                << " (frame " << g_frameCountForCrash << ")" << std::endl;
        if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
            ULONG_PTR rw = ep->ExceptionRecord->ExceptionInformation[0];
            ULONG_PTR target = ep->ExceptionRecord->ExceptionInformation[1];
            g_weLog << "  Access violation: " << (rw == 0 ? "read" : "write")
                    << " at 0x" << std::hex << target << std::dec << std::endl;
        }
        g_weLog.flush();
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// SEH wrapper for full frame body (SimBridge::update, camera, Run, etc.)
// Must be separate from C++ try/catch
static DWORD g_frameSEHCode = 0;
static void* g_frameSEHAddr = nullptr;

int runWickedEngine(const std::string& userFolder, const ScenarioData& scenarioData,
                    int width, int height, bool fullscreen) {
    // Install global crash handler FIRST (before any WE code runs)
    SetUnhandledExceptionFilter(weCrashHandler);

    // Open crash diagnostic log
    g_weLog.open("wicked_engine.log", std::ios::out | std::ios::trunc);

    const std::string& worldName = scenarioData.worldName;
    weLog("Starting Wicked Engine backend (DX12)...");
    weLog("  Scenario: " + scenarioData.scenarioName);
    weLog("  World: " + worldName);
    weLog("  Window: " + std::to_string(width) + "x" + std::to_string(height) +
          (fullscreen ? " fullscreen" : " windowed"));

    // Resolve world path
    std::string worldPath = "World/" + worldName + "/";
    if (!Utilities::pathExists(worldPath)) {
        worldPath = userFolder + "World/" + worldName + "/";
    }
    if (!Utilities::pathExists(worldPath)) {
        weLogErr("World not found: " + worldName);
        g_weLog.close();
        return EXIT_FAILURE;
    }
    weLog("  World path: " + worldPath);

    // --- Win32 window creation ---
    HINSTANCE hInstance = GetModuleHandle(nullptr);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW wcex = {};
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WickedWndProc;
    wcex.hInstance = hInstance;
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszClassName = L"BridgeCommandWicked";
    RegisterClassExW(&wcex);

    DWORD style = fullscreen ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    RECT rc = {0, 0, (LONG)width, (LONG)height};
    AdjustWindowRect(&rc, style, FALSE);

    std::wstring windowTitle = L"Bridge Command - " +
        std::wstring(scenarioData.scenarioName.begin(), scenarioData.scenarioName.end()) +
        L" (Wicked Engine DX12)";
    HWND hWnd = CreateWindowW(
        wcex.lpszClassName,
        windowTitle.c_str(),
        style,
        CW_USEDEFAULT, 0,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInstance, nullptr);

    if (!hWnd) {
        weLogErr("Failed to create Win32 window for Wicked Engine");
        g_weLog.close();
        return EXIT_FAILURE;
    }
    weLog("  Win32 window created.");
    ShowWindow(hWnd, SW_SHOW);
    // Force window to front even if process lost foreground status
    // (topmost-then-notopmost trick works on Windows 10/11)
    SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetWindowPos(hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetForegroundWindow(hWnd);
    UpdateWindow(hWnd);

    // --- Initialize Wicked Engine ---
    wi::Application application;
    g_weApp = &application;
    application.SetWindow(hWnd);

    // Set shader path
    std::string shaderPath;
    std::vector<std::string> shaderSearchPaths = {
        "../../WickedEngine/WickedEngine/shaders/",
        "../WickedEngine/WickedEngine/shaders/",
        "../WickedEngine/shaders/",
    };
    const char* envShaderPath = std::getenv("WE_SHADER_PATH");
    if (envShaderPath) {
        shaderSearchPaths.insert(shaderSearchPaths.begin(), std::string(envShaderPath));
    }
    for (const auto& p : shaderSearchPaths) {
        if (Utilities::pathExists(p + "objectVS.hlsl") || Utilities::pathExists(p + "globals.hlsli")) {
            shaderPath = p;
            break;
        }
    }
    if (!shaderPath.empty()) {
        wi::renderer::SetShaderPath(shaderPath);
        weLog("  Shader path: " + shaderPath);
    } else {
        weLogErr("Could not find WickedEngine shader directory.");
    }

    // Create 3D render path
    BCRenderPath renderPath;
    renderPath.setSSREnabled(true);
    renderPath.setFXAAEnabled(true);
    renderPath.setBloomEnabled(true);
    renderPath.setLensFlareEnabled(true);  // depth-tested screen-space flares for nav lights
    application.ActivatePath(&renderPath);

    application.infoDisplay.active = true;
    application.infoDisplay.watermark = true;
    application.infoDisplay.resolution = true;
    application.infoDisplay.fpsinfo = true;

    // Run a few frames to let WE initialize its graphics device, job system, and shaders.
    // Scene setup (mesh creation, ocean, materials) requires the GPU device to be ready.
    weLog("  Initializing Wicked Engine (10 warmup frames)...");
    for (int i = 0; i < 10; i++) {
        MSG initMsg = {};
        while (PeekMessage(&initMsg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&initMsg);
            DispatchMessage(&initMsg);
        }
        application.Run();
    }
    weLog("  Wicked Engine initialized.");

    // --- Initialize ImGui overlay ---
    bc::graphics::wicked::ImGuiInit(hWnd);
    ImGui_ImplWin32_Init(hWnd);  // Win32 platform backend for proper input
    bc::gui::ImGuiOverlay overlay;
    overlay.init(width, height);

    // --- ESC menu + Settings panel + Radar display ---
    bool showEscMenu = false;
    bool showSettings = false;
    bool showRadarFullscreen = false;
    bool showEcdisFullscreen = false;
    bc::gui::SettingsPanel settingsPanel;
    settingsPanel.load(userFolder);
    bc::gui::RadarDisplay radarDisplay;
    bc::gui::EcdisDisplay ecdisDisplay;
    ecdisDisplay.init(Utilities::getUserDirBase() + "tilecache/");
    weLog("  ImGui overlay initialized.");

    // --- Initialize Sound ---
    Sound sound;
    sound.load("Sounds/Engine.wav", "Sounds/Bwave.wav",
               "Sounds/horn.wav", "Sounds/Alarm.wav");
    sound.setVolumeEngine(0.0f);
    sound.setVolumeWave(0.3f);
    sound.setVolumeHorn(0.0f);
    sound.setVolumeAlarm(0.0f);
    sound.StartSound();
    weLog("  Sound system initialized.");

    // --- Set up the scene ---
    wi::scene::Scene& scene = wi::scene::GetScene();
    wi::scene::CameraComponent& camera = wi::scene::GetCamera();

    // Declare variables needed after scene setup (outside try block)
    CoordConverter coords;
    std::unique_ptr<bc::graphics::wicked::WickedTerrainNode> terrainNode;
    bc::graphics::wicked::WickedMultiCascadeOcean ocean;

    float ownShipX = 0, ownShipZ = 0;
    float ownShipHeading = scenarioData.ownShipData.initialBearing;
    float ownShipSpeed = scenarioData.ownShipData.initialSpeed; // knots
    float ownShipRudder = 0; // wheel angle degrees (-30 to 30)
    float ownShipPortEngine = 0; // -1.0 to 1.0
    float ownShipStbdEngine = 0; // -1.0 to 1.0
    float maxSpeedAhead = 14.0f; // knots, read from boat.ini
    float ownShipBowThruster = 0; // -1.0 to 1.0
    float beaufortScale = 3.0f; // sea state for wave heading disturbance
    wi::ecs::Entity sunEntity = wi::ecs::INVALID_ENTITY; // directional sun light
    float sunRise = 6.0f, sunSet = 18.0f; // hours (0-24), persisted for day/night cycle
    float ownShipScaleFactor = 1.0f;
    float ownShipHeightCorr = 0;
    float cameraViewX = 0, cameraViewY = 10.0f, cameraViewZ = 0; // bridge position in world coords
    float viewLocalX = 0, viewLocalY = 0, viewLocalZ = 0; // bridge view in ship-local coords (pre-scale)
    float bridgeHalfW = 3.0f, bridgeHalfD = 3.0f; // walkable bridge bounds (meters, ship-local)
    wi::ecs::Entity ownShipEntity = wi::ecs::INVALID_ENTITY; // stored to toggle visibility

    // Radar screen
    static const int RADAR_TEX_SIZE = 1024;
    static uint8_t radarPixels[RADAR_TEX_SIZE * RADAR_TEX_SIZE * 4];
    wi::ecs::Entity radarScreenEntity = wi::ecs::INVALID_ENTITY;
    wi::ecs::Entity radarMaterialEntity = wi::ecs::INVALID_ENTITY;
    float radarLocalX = 0, radarLocalY = 0, radarLocalZ = 0; // ship-local (scaled)
    float radarScreenSize = 1.0f; // metres
    float radarScreenTilt = 0.0f; // degrees

    // Map screen (ECDIS)
    static const int MAP_TEX_SIZE = 512;
    std::unique_ptr<MapScreen> mapScreen;
    wi::ecs::Entity mapScreenEntity = wi::ecs::INVALID_ENTITY;
    wi::ecs::Entity mapMaterialEntity = wi::ecs::INVALID_ENTITY;
    float mapLocalX = 0, mapLocalY = 0, mapLocalZ = 0;
    float mapScreenSizeVal = 0;
    float mapScreenTilt = 0;
    float mapScreenAngle = 0; // yaw offset in degrees (negative = face port/center)
    int mapZoom = 14;

    // Other ship movement state
    struct OtherShipState {
        wi::ecs::Entity entity = wi::ecs::INVALID_ENTITY;
        float x, z;           // current world position
        float heading;         // current heading (degrees)
        float speed;           // current speed (knots)
        float heightCorr;      // Y position
        float scaleFactor;
        int currentLeg;        // index into legs
        float distTravelled;   // nautical miles along current leg
    };
    std::vector<OtherShipState> otherShipStates;

    // Buoy state for per-frame tidal updates
    struct BuoyState {
        wi::ecs::Entity entity = wi::ecs::INVALID_ENTITY;
        float heightCorr;
        float scaleFactor;
    };
    std::vector<BuoyState> buoyStates;

    // Navigation light: WE point light with lens flare for distance visibility.
    // Lens flare is depth-tested (auto-occluded by hull geometry) and gated by
    // setting intensity=0 when out of arc/range/flash-off.
    struct WENavLight {
        wi::ecs::Entity entity = wi::ecs::INVALID_ENTITY;       // point light
        int shipIndex = -1;      // index into otherShipStates, -1 for own ship, -2 for fixed/buoy
        int buoyIndex = -1;      // >= 0: buoyStates index (light attached to buoy)
        float localX = 0, localY = 0, localZ = 0; // ship-relative pos OR world pos (if fixed)
        float r = 1, g = 1, b = 1; // color (0-1)
        float startAngle = 0, endAngle = 360; // directional arc (degrees)
        float range = 5000;      // visibility range (metres)
        float intensity_cd = 12;  // luminous intensity (candela), from COLREG Annex I
        std::string sequence;    // flash pattern ('D' = dark)
        float charTime = 0.25f;  // seconds per sequence character
        float timeOffset = 0;    // random phase offset
    };
    std::vector<WENavLight> navLights;

    try { // Wrap scene setup in try-catch to diagnose crashes

    // ===== LENS FLARE TEXTURES (colored per COLREG: red, green, white) =====
    loadNavLightFlareTextures();

    // ===== SUN / LIGHTING =====
    pumpMessages(); // Keep window responsive during setup
    weLog("  Setting up sun/lighting...");
    sunEntity = scene.Entity_CreateLight("Sun");
    wi::scene::LightComponent* sunLight = scene.lights.GetComponent(sunEntity);
    if (sunLight) {
        sunLight->SetType(wi::scene::LightComponent::DIRECTIONAL);
        sunLight->intensity = 8.0f;
        sunLight->SetCastShadow(true);
    }
    // Store sunrise/sunset for dynamic updates
    sunRise = scenarioData.sunRise > 0 ? scenarioData.sunRise : 6.0f;
    sunSet  = scenarioData.sunSet  > 0 ? scenarioData.sunSet  : 18.0f;
    // Position sun based on start time.
    // WE computes light.direction by transforming (0,1,0) through the world matrix,
    // so we build a quaternion that maps (0,1,0) to the desired sun direction.
    {
        float timeOfDay = scenarioData.startTime; // hours (0-24) in simulation path
        float dayLength = sunSet - sunRise;
        float sunProgress = (dayLength > 0) ? (timeOfDay - sunRise) / dayLength : 0.5f;
        sunProgress = std::max(0.0f, std::min(1.0f, sunProgress));

        // Sun traces a semicircle: east horizon -> zenith -> west horizon
        // X = east/west, Y = up, Z = south bias (northern hemisphere)
        float sdx = -std::cos((float)M_PI * sunProgress);
        float sdy =  std::sin((float)M_PI * sunProgress);
        float sdz = -0.3f; // slight southward arc
        float slen = std::sqrt(sdx * sdx + sdy * sdy + sdz * sdz);
        sdx /= slen; sdy /= slen; sdz /= slen;

        // Quaternion from (0,1,0) to (sdx, sdy, sdz) via axis-angle
        using namespace DirectX;
        XMVECTOR src = XMVectorSet(0, 1, 0, 0);
        XMVECTOR dst = XMVectorSet(sdx, sdy, sdz, 0);
        XMVECTOR axis = XMVector3Cross(src, dst);
        float axLen = XMVectorGetX(XMVector3Length(axis));
        XMVECTOR quat;
        if (axLen < 0.0001f) {
            quat = XMQuaternionIdentity();
        } else {
            axis = XMVector3Normalize(axis);
            float dot = sdy; // dot((0,1,0), normalized dir)
            float angle = std::acos(std::max(-1.0f, std::min(1.0f, dot)));
            quat = XMQuaternionRotationAxis(axis, angle);
        }

        wi::scene::TransformComponent* sunTransform = scene.transforms.GetComponent(sunEntity);
        if (sunTransform) {
            sunTransform->ClearTransform();
            XMStoreFloat4(&sunTransform->rotation_local, quat);
            sunTransform->SetDirty();
            sunTransform->UpdateTransform();
        }
    }

    // ===== ATMOSPHERE / WEATHER =====
    scene.weather.SetRealisticSky(true);
    scene.weather.SetVolumetricClouds(true);
    scene.weather.SetRealisticSkyAerialPerspective(true);
    scene.weather.SetHeightFog(true);
    scene.weather.SetVolumetricCloudsCastShadow(true);
    scene.weather.SetRealisticSkyReceiveShadow(true);
    scene.weather.ambient = DirectX::XMFLOAT3(0.3f, 0.35f, 0.4f);
    scene.weather.skyExposure = 1.2f;

    // Maritime atmosphere: ocean is dark (~0.06 albedo), more Mie scattering
    // from sea spray aerosols, and forward-scattering haze near the horizon.
    auto& atmo = scene.weather.atmosphereParameters;
    atmo.groundAlbedo = XMFLOAT3(0.06f, 0.08f, 0.10f); // dark ocean, slight blue-green
    // Baseline maritime Mie: ~2x continental due to sea salt aerosols
    float beaufortInit = std::max(0.0f, scenarioData.weather);
    float mieFactor = 1.0f + beaufortInit / 12.0f; // 1.0 at B0, 2.0 at B12
    float mieBase = 0.006f * mieFactor;
    atmo.mieScattering = XMFLOAT3(mieBase, mieBase, mieBase);
    float mieExt = mieBase * 1.11f; // extinction slightly > scattering (absorption)
    atmo.mieExtinction = XMFLOAT3(mieExt, mieExt, mieExt);
    float mieAbs = mieExt - mieBase;
    atmo.mieAbsorption = XMFLOAT3(mieAbs, mieAbs, mieAbs);
    atmo.aerialPerspectiveScale = 1.5f + beaufortInit * 0.1f; // more haze in rough weather

    // Height fog: maritime sea-level fog, hugs surface
    float visRange = scenarioData.visibilityRange;
    if (visRange <= 0) visRange = 10.0f; // default 10 nm
    scene.weather.fogHeightStart = 0.0f;  // sea level
    scene.weather.fogHeightEnd = 30.0f + (10.0f - std::min(visRange, 10.0f)) * 20.0f; // 30-230m
    float fogDistMeters = visRange * 1852.0f; // nm to meters
    scene.weather.fogStart = fogDistMeters * 0.3f;
    if (visRange < 5.0f) {
        scene.weather.fogDensity = 0.01f / std::max(visRange, 0.1f);
    }

    // Rain: WE's rain particle emitter crashes (SEH 0xC0000005) when
    // rain_amount > 0, both at init and mid-gameplay. Disabled for now.
    // Rain intensity is still used for cloud darkening and radar clutter.
    scene.weather.rain_amount = 0.0f;

    // ===== VOLUMETRIC CLOUDS =====
    // Maritime cloud setup. Two drivers for coverage:
    //   1. Beaufort (wind-driven convective clouds)
    //   2. Low visibility (overcast stratus -- typical British grey day)
    // This decouples cloud cover from wind, so B3 + 3nm vis = full overcast.
    {
        auto& vc = scene.weather.volumetricCloudParameters;

        // Visibility-driven overcast: vis < 8nm implies cloud cover
        float visCoverage = std::max(0.0f, 1.0f - visRange / 8.0f); // 1.0 at 0nm, 0 at 8nm
        float beaufortCoverage = std::min(1.0f, beaufortInit / 8.0f);
        float overcastFactor = std::max(visCoverage, beaufortCoverage);
        bool isOvercast = visCoverage > beaufortCoverage; // vis driving clouds, not wind

        // Cloud base: overcast stratus sits low (~300-600m), convective cumulus higher
        if (isOvercast) {
            vc.cloudStartHeight = 600.0f - visCoverage * 300.0f; // 600m -> 300m
            vc.cloudThickness = 1500.0f + visCoverage * 1000.0f; // thin stratus layer
        } else {
            vc.cloudStartHeight = 1500.0f - beaufortInit * 75.0f; // 1500m -> 600m
            vc.cloudThickness = 4000.0f + beaufortInit * 500.0f;  // taller with wind
        }

        // Phase functions: forward scattering for silver lining effect
        vc.phaseG = 0.6f;
        vc.phaseG2 = -0.3f;
        vc.phaseBlend = 0.3f;

        // Multi-scattering for realistic cloud lighting
        vc.multiScatteringScattering = 1.0f;
        vc.multiScatteringExtinction = 0.1f;
        vc.multiScatteringEccentricity = 0.2f;

        // Ambient ground contribution: higher for overcast (light filters through)
        vc.ambientGroundMultiplier = isOvercast ? 0.75f : 0.6f;

        // Horizon blending for distant cloud-sky merge
        vc.horizonBlendAmount = 0.0000125f;
        vc.horizonBlendPower = 2.0f;

        // Shadow from clouds onto scene
        vc.shadowStepLength = 3000.0f;

        // Primary cloud layer
        auto& L1 = vc.layerFirst;

        // Coverage: WE default is 1.0 which gives scattered clouds.
        // For overcast, we need well above 1.0 plus a high minimum floor
        // to eliminate clear patches. coverageAmount drives the weather noise
        // threshold -- higher = more area passes = more cloud fill.
        if (isOvercast) {
            L1.coverageAmount = 1.5f + visCoverage * 0.5f;   // 1.5 to 2.0
            L1.coverageMinimum = 0.5f + visCoverage * 0.3f;  // 0.5 to 0.8 floor
        } else {
            L1.coverageAmount = 0.5f + beaufortCoverage * 1.0f; // 0.5 to 1.5
            L1.coverageMinimum = 0.0f;
        }

        // Cloud type: overcast = small flat stratus, storm = large cumulonimbus
        // Only push to large type at B7+, otherwise keep flat
        if (beaufortInit > 7.0f) {
            L1.typeAmount = std::min(1.0f, (beaufortInit - 7.0f) / 5.0f);
        } else {
            L1.typeAmount = 0.0f; // flat stratus/stratocumulus
        }
        L1.typeMinimum = 0.0f;

        // Albedo: overcast stays light grey, only darken in storms (B6+)
        float stormDarken = std::max(0.0f, (beaufortInit - 6.0f) / 6.0f);
        float cloudAlbedo = 0.9f - 0.15f * stormDarken;
        L1.albedo = XMFLOAT3(cloudAlbedo, cloudAlbedo, cloudAlbedo);

        // Extinction: thin for overcast stratus, denser for storm clouds
        float ext = isOvercast ? 0.05f : (0.071f + 0.03f * stormDarken);
        L1.extinctionCoefficient = XMFLOAT3(0.71f * ext, 0.86f * ext, 1.0f * ext);

        // Noise scales: overcast needs smoother coverage with fewer gaps
        if (isOvercast) {
            L1.totalNoiseScale = 0.0003f;   // smoother uniform sheet
            L1.weatherScale = 0.000005f;     // reduce large-scale gaps
        } else {
            L1.totalNoiseScale = 0.0005f;    // WE-like scattered
            L1.weatherScale = 0.00002f;      // default
        }
        L1.curlScale = 0.3f;
        L1.curlNoiseHeightFraction = 5.0f;
        L1.curlNoiseModifier = 500.0f;
        L1.detailScale = 4.0f;
        L1.detailNoiseHeightFraction = 10.0f;
        L1.detailNoiseModifier = isOvercast ? 0.15f : 0.3f; // less detail erosion for overcast

        // Gradients: shape profiles for cloud types
        // Small: flat fair-weather cumulus
        L1.gradientSmall = XMFLOAT4(0.01f, 0.1f, 0.11f, 0.2f);
        // Medium: standard cumulus
        L1.gradientMedium = XMFLOAT4(0.01f, 0.08f, 0.3f, 0.4f);
        // Large: towering cumulus / cumulonimbus
        L1.gradientLarge = XMFLOAT4(0.01f, 0.06f, 0.75f, 0.95f);

        // Anvil deformation for cumulonimbus tops
        L1.anvilDeformationSmall = XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
        L1.anvilDeformationMedium = XMFLOAT4(15.0f, 0.1f, 15.0f, 0.1f);
        L1.anvilDeformationLarge = XMFLOAT4(5.0f, 0.25f, 5.0f, 0.15f);

        // Wind animation
        L1.skewAlongWindDirection = 700.0f;
        L1.skewAlongCoverageWindDirection = 2500.0f;
    }

    // ===== OCEAN =====
    weLog("  Setting up ocean...");
    float beaufort = scenarioData.weather;
    if (beaufort < 0) beaufort = 3.0f;
    beaufortScale = beaufort;
    try {
        ocean.load(&scene, beaufort);
        weLog("  Ocean initialized (Beaufort " + std::to_string(beaufort) + ")");
    } catch (const std::exception& e) {
        weLogErr("Ocean setup failed: " + std::string(e.what()));
    } catch (...) {
        weLogErr("Ocean setup failed (unknown error)");
    }

    // ===== TERRAIN =====
    weLog("  Setting up terrain...");
    std::string terrainFile = worldPath + "terrain.ini";
    if (Utilities::pathExists(terrainFile)) {
        int terrainCount = IniFile::iniFileTou32(terrainFile, "Number");
        if (terrainCount > 0) {
            // Load primary terrain (terrain 1) for coordinate reference
            std::string hmName = IniFile::iniFileToString(terrainFile, IniFile::enumerate1("HeightMap", 1));
            std::string txName = IniFile::iniFileToString(terrainFile, IniFile::enumerate1("Texture", 1));
            float tLat = IniFile::iniFileTof32(terrainFile, IniFile::enumerate1("TerrainLat", 1));
            float tLon = IniFile::iniFileTof32(terrainFile, IniFile::enumerate1("TerrainLong", 1));
            float tLatExt = IniFile::iniFileTof32(terrainFile, IniFile::enumerate1("TerrainLatExtent", 1));
            float tLonExt = IniFile::iniFileTof32(terrainFile, IniFile::enumerate1("TerrainLongExtent", 1));
            float tMaxH = IniFile::iniFileTof32(terrainFile, IniFile::enumerate1("TerrainMaxHeight", 1));
            float tSeaD = IniFile::iniFileTof32(terrainFile, IniFile::enumerate1("SeaMaxDepth", 1));

            // Initialize coordinate converter using primary terrain
            coords.init(tLon, tLat, tLonExt, tLatExt);

            bc::graphics::wicked::TerrainTileConfig config;
            config.heightmapPath = worldPath + hmName;
            config.texturePath = worldPath + txName;
            config.latitude = tLat;
            config.longitude = tLon;
            config.latExtent = tLatExt;
            config.lonExtent = tLonExt;
            config.maxHeight = tMaxH;
            config.seaMaxDepth = tSeaD;

            // Check for F32 heightmap rows/cols
            config.heightmapRows = IniFile::iniFileTou32(terrainFile, IniFile::enumerate1("TerrainHeightMapRows", 1));
            config.heightmapCols = IniFile::iniFileTou32(terrainFile, IniFile::enumerate1("TerrainHeightMapColumns", 1));
            if (config.heightmapRows == 0 || config.heightmapCols == 0) {
                int legacySize = IniFile::iniFileTou32(terrainFile, IniFile::enumerate1("TerrainHeightMapSize", 1));
                if (legacySize > 0) {
                    config.heightmapRows = legacySize;
                    config.heightmapCols = legacySize;
                }
            }

            config.usesRGB = IniFile::iniFileTou32(terrainFile, IniFile::enumerate1("UsesRGB", 1)) > 0;

            // Use terrain SW corner as reference point to align with CoordConverter
            // (CoordConverter.longToX/latToZ are relative to tLon/tLat)
            float refLon = tLon;
            float refLat = tLat;

            terrainNode = std::make_unique<bc::graphics::wicked::WickedTerrainNode>(&scene);
            if (terrainNode->loadFromConfig(config, refLon, refLat)) {
                weLog("  Terrain loaded: " + config.heightmapPath);
                weLog("    texture: " + config.texturePath);
                weLog("    worldWidth=" + std::to_string(coords.terrainXWidth) +
                      " worldDepth=" + std::to_string(coords.terrainZWidth));
                weLog("    position=(" + std::to_string(terrainNode->getPosition().x) +
                      "," + std::to_string(terrainNode->getPosition().y) +
                      "," + std::to_string(terrainNode->getPosition().z) + ")");
                weLog("    usesRGB=" + std::to_string(config.usesRGB ? 1 : 0) +
                      " maxHeight=" + std::to_string(config.maxHeight) +
                      " seaMaxDepth=" + std::to_string(config.seaMaxDepth));
                // Sample terrain heights -- comprehensive scan to verify heightmap loaded correctly
                float midX = coords.terrainXWidth / 2.0f;
                float midZ = coords.terrainZWidth / 2.0f;
                weLog("    heightAt(0,0)=" + std::to_string(terrainNode->getHeightAt(0, 0)) +
                      " heightAt(mid,mid)=" + std::to_string(terrainNode->getHeightAt(midX, midZ)) +
                      " heightAt(max,max)=" + std::to_string(terrainNode->getHeightAt(coords.terrainXWidth, coords.terrainZWidth)));

                // Scan full heightmap for min/max/land statistics
                {
                    float hMin = 1e9f, hMax = -1e9f;
                    int landPixels = 0, seaPixels = 0, totalPixels = 0;
                    float step = coords.terrainXWidth / 50.0f; // ~50x50 sample grid
                    float stepZ = coords.terrainZWidth / 50.0f;
                    for (float sz = 0; sz <= coords.terrainZWidth; sz += stepZ) {
                        for (float sx = 0; sx <= coords.terrainXWidth; sx += step) {
                            float h = terrainNode->getHeightAt(sx, sz);
                            if (h < hMin) hMin = h;
                            if (h > hMax) hMax = h;
                            if (h > 0.0f) landPixels++;
                            else seaPixels++;
                            totalPixels++;
                        }
                    }
                    weLog("    heightmap stats: min=" + std::to_string(hMin) +
                          " max=" + std::to_string(hMax) +
                          " land=" + std::to_string(landPixels) +
                          " sea=" + std::to_string(seaPixels) +
                          " of " + std::to_string(totalPixels) + " samples");
                }
            } else {
                weLogErr("Failed to load terrain heightmap");
                terrainNode.reset();
            }
        }
    } else {
        weLog("  No terrain.ini found, running without terrain");
    }

    // ===== OWN SHIP =====
    pumpMessages();
    weLog("  Setting up own ship...");
    if (!scenarioData.ownShipData.ownShipName.empty()) {
        std::string shipName = scenarioData.ownShipData.ownShipName;
        std::string basePath = resolveModelPath("Models/Ownship/" + shipName + "/",
                                                 userFolder, "");

        std::string boatIni = basePath + "boat.ini";
        std::string modelFileName = IniFile::iniFileToString(boatIni, "FileName");
        float scaleFactor = IniFile::iniFileTof32(boatIni, "ScaleFactor");
        if (scaleFactor <= 0) scaleFactor = 1.0f;

        float yCorrection = IniFile::iniFileTof32(boatIni, "YCorrection");
        float heightCorrection = yCorrection * scaleFactor;
        ownShipScaleFactor = scaleFactor;
        ownShipHeightCorr = heightCorrection;
        maxSpeedAhead = IniFile::iniFileTof32(boatIni, "maxSpeedAhead");
        if (maxSpeedAhead <= 0) maxSpeedAhead = 14.0f;

        // Get camera view position (first view = bridge view)
        uint32_t numViews = (uint32_t)IniFile::iniFileTof32(boatIni, "Views");
        if (numViews > 0) {
            viewLocalX = IniFile::iniFileTof32(boatIni, IniFile::enumerate1("ViewX", 1));
            viewLocalY = IniFile::iniFileTof32(boatIni, IniFile::enumerate1("ViewY", 1));
            viewLocalZ = IniFile::iniFileTof32(boatIni, IniFile::enumerate1("ViewZ", 1));
        }

        // Compute walkable bridge bounds from view positions (in scaled meters)
        // Bridge views at similar Y to view 1 define the walkable area
        {
            float maxDx = 0, maxDz = 0;
            for (uint32_t v = 2; v <= numViews; v++) {
                float vHigh = IniFile::iniFileTof32(boatIni, IniFile::enumerate1("ViewHigh", v));
                if (vHigh > 0.5f) continue; // skip overhead/high views
                float vx = IniFile::iniFileTof32(boatIni, IniFile::enumerate1("ViewX", v));
                float vy = IniFile::iniFileTof32(boatIni, IniFile::enumerate1("ViewY", v));
                float vz = IniFile::iniFileTof32(boatIni, IniFile::enumerate1("ViewZ", v));
                // Only include views near bridge deck height (within 10 model units)
                if (std::abs(vy - viewLocalY) > 10.0f) continue;
                float dx = std::abs(vx - viewLocalX) * scaleFactor;
                float dz = std::abs(vz - viewLocalZ) * scaleFactor;
                if (dx > maxDx) maxDx = dx;
                if (dz > maxDz) maxDz = dz;
            }
            // Add margin beyond wing positions (1m width, 0.5m depth)
            bridgeHalfW = std::max(3.0f, maxDx + 1.0f);
            bridgeHalfD = std::max(1.5f, maxDz + 0.5f); // tight fore-aft to prevent walking through windows
            weLog("  Bridge walk bounds: +-" + std::to_string(bridgeHalfW) +
                  "m x +-" + std::to_string(bridgeHalfD) + "m");
        }

        // Convert own ship lat/lon to world coordinates
        ownShipX = coords.longToX(scenarioData.ownShipData.initialLong);
        ownShipZ = coords.latToZ(scenarioData.ownShipData.initialLat);

        // Compute bridge view position in world coords
        float headRad = ownShipHeading * (float)M_PI / 180.0f;
        float vxScaled = viewLocalX * scaleFactor;
        float vzScaled = viewLocalZ * scaleFactor;
        cameraViewX = ownShipX + vxScaled * std::cos(headRad) + vzScaled * std::sin(headRad);
        cameraViewY = heightCorrection + viewLocalY * scaleFactor;
        cameraViewZ = ownShipZ - vxScaled * std::sin(headRad) + vzScaled * std::cos(headRad);

        // Load ship model
        std::string modelPath = basePath + modelFileName;
        ownShipEntity = loadModelOrPlaceholder(scene, modelPath,
                                               "OwnShip", 0.5f, 0.5f, 0.6f, 20.0f,
                                               true /*allowTransparency: ship windows*/);
        setEntityTransform(scene, ownShipEntity, ownShipX, heightCorrection, ownShipZ,
                           ownShipHeading, scaleFactor);
        weLog("  Own ship: " + shipName + " at (" +
              std::to_string(ownShipX) + ", " + std::to_string(ownShipZ) +
              ") heading " + std::to_string(ownShipHeading));

        // Read radar screen position from boat.ini
        float rsx = IniFile::iniFileTof32(boatIni, "RadarScreenX");
        float rsy = IniFile::iniFileTof32(boatIni, "RadarScreenY");
        float rsz = IniFile::iniFileTof32(boatIni, "RadarScreenZ");
        radarScreenSize = IniFile::iniFileTof32(boatIni, "RadarScreenSize");
        radarScreenTilt = IniFile::iniFileTof32(boatIni, "RadarScreenTilt");
        if (radarScreenSize <= 0) radarScreenSize = 1.0f;
        radarLocalX = rsx * scaleFactor;
        radarLocalY = rsy * scaleFactor + heightCorrection;
        radarLocalZ = rsz * scaleFactor;
        float screenMetres = radarScreenSize * scaleFactor * 0.5f; // halve to fit model's screen surface

        // 3D radar screen disabled: the quad clips through bridge console geometry.
        // Fullscreen radar overlay (R key) still works via RadarDisplay.
        if (false && (rsx != 0 || rsy != 0 || rsz != 0)) {
            // Initialize radar pixels to black (will be filled by RadarCalculation)
            memset(radarPixels, 0, sizeof(radarPixels));
            for (int i = 0; i < RADAR_TEX_SIZE * RADAR_TEX_SIZE; i++) {
                radarPixels[i * 4 + 3] = 255; // A = opaque
            }

            // Create initial GPU texture (will be recreated each radar update)
            {
                wi::graphics::TextureDesc desc;
                desc.width = RADAR_TEX_SIZE;
                desc.height = RADAR_TEX_SIZE;
                desc.format = wi::graphics::Format::R8G8B8A8_UNORM;
                desc.bind_flags = wi::graphics::BindFlag::SHADER_RESOURCE;
                desc.mip_levels = 1;
                desc.array_size = 1;
                wi::graphics::SubresourceData initData;
                initData.data_ptr = radarPixels;
                initData.row_pitch = RADAR_TEX_SIZE * 4;
                initData.slice_pitch = initData.row_pitch * RADAR_TEX_SIZE;
                wi::graphics::GetDevice()->CreateTexture(&desc, &initData, &g_radarTex);
                weLog("  Radar texture: " + std::string(g_radarTex.IsValid() ? "OK" : "FAILED"));
            }

            // Create emissive material (self-lit screen, not affected by scene lighting)
            radarMaterialEntity = scene.Entity_CreateMaterial("radar_screen_mat");
            auto* radarMat = scene.materials.GetComponent(radarMaterialEntity);
            if (radarMat) {
                radarMat->baseColor = DirectX::XMFLOAT4(0, 0, 0, 1);
                radarMat->roughness = 0.2f;
                radarMat->metalness = 0.0f;
                radarMat->SetEmissiveStrength(3.0f);
                radarMat->emissiveColor = DirectX::XMFLOAT4(1, 1, 1, 1);
                radarMat->textures[wi::scene::MaterialComponent::EMISSIVEMAP].resource.SetTexture(g_radarTex);
                radarMat->SetDoubleSided(true);
                radarMat->CreateRenderData();
            }

            // Create quad mesh (two triangles, tilted by RadarScreenTilt)
            wi::ecs::Entity meshEntity = scene.Entity_CreateMesh("radar_screen_mesh");
            auto* mesh = scene.meshes.GetComponent(meshEntity);
            if (mesh) {
                float half = screenMetres * 0.5f;
                float tiltRad = radarScreenTilt * (float)M_PI / 180.0f;
                float cosT = std::cos(tiltRad);
                float sinT = std::sin(tiltRad);

                // Quad corners in local space (tilted around X axis)
                // Bottom-left, bottom-right, top-right, top-left
                mesh->vertex_positions.push_back(DirectX::XMFLOAT3(-half, -half * cosT, -half * sinT));
                mesh->vertex_positions.push_back(DirectX::XMFLOAT3( half, -half * cosT, -half * sinT));
                mesh->vertex_positions.push_back(DirectX::XMFLOAT3( half,  half * cosT,  half * sinT));
                mesh->vertex_positions.push_back(DirectX::XMFLOAT3(-half,  half * cosT,  half * sinT));

                DirectX::XMFLOAT3 normal(0, sinT, -cosT);
                for (int n = 0; n < 4; n++)
                    mesh->vertex_normals.push_back(normal);

                mesh->vertex_uvset_0.push_back(DirectX::XMFLOAT2(0, 1));
                mesh->vertex_uvset_0.push_back(DirectX::XMFLOAT2(1, 1));
                mesh->vertex_uvset_0.push_back(DirectX::XMFLOAT2(1, 0));
                mesh->vertex_uvset_0.push_back(DirectX::XMFLOAT2(0, 0));

                mesh->indices = {0, 1, 2, 0, 2, 3};

                mesh->subsets.push_back(wi::scene::MeshComponent::MeshSubset());
                mesh->subsets.back().materialID = radarMaterialEntity;
                mesh->subsets.back().indexOffset = 0;
                mesh->subsets.back().indexCount = 6;
                mesh->CreateRenderData();
            }

            // Create radar screen entity with transform
            radarScreenEntity = wi::ecs::CreateEntity();
            scene.transforms.Create(radarScreenEntity);
            scene.names.Create(radarScreenEntity) = "RadarScreen";

            wi::ecs::Entity radarObj = scene.Entity_CreateObject("radar_screen_obj");
            scene.Component_Attach(radarObj, radarScreenEntity);
            auto* obj = scene.objects.GetComponent(radarObj);
            if (obj) obj->meshID = meshEntity;

            weLog("  Radar screen at (" + std::to_string(rsx) + "," +
                  std::to_string(rsy) + "," + std::to_string(rsz) +
                  ") size=" + std::to_string(radarScreenSize) +
                  " tilt=" + std::to_string(radarScreenTilt));
        }

        // ===== MAP SCREEN (ECDIS) =====
        {
            float msx = IniFile::iniFileTof32(boatIni, "MapScreenX");
            float msy = IniFile::iniFileTof32(boatIni, "MapScreenY");
            float msz = IniFile::iniFileTof32(boatIni, "MapScreenZ");
            mapScreenSizeVal = IniFile::iniFileTof32(boatIni, "MapScreenSize");
            mapScreenTilt = IniFile::iniFileTof32(boatIni, "MapScreenTilt");
            mapScreenAngle = IniFile::iniFileTof32(boatIni, "MapScreenAngle");
            mapZoom = IniFile::iniFileTou32(boatIni, "MapScreenZoom");
            if (mapScreenSizeVal <= 0) mapScreenSizeVal = 0;
            if (mapZoom < 10 || mapZoom > 17) mapZoom = 14;

            // 3D map screen disabled: clips through bridge console geometry.
            // Fullscreen ECDIS overlay (M key) still works via EcdisDisplay.
            if (false && mapScreenSizeVal > 0 && (msx != 0 || msy != 0 || msz != 0)) {
                mapLocalX = msx * scaleFactor;
                mapLocalY = msy * scaleFactor + heightCorrection;
                mapLocalZ = msz * scaleFactor;
            float mapMetres = mapScreenSizeVal * scaleFactor * 0.5f;

            // Init MapScreen tile downloaders
            mapScreen = std::make_unique<MapScreen>(MAP_TEX_SIZE);
            mapScreen->init(Utilities::getUserDirBase() + "tilecache/");

            // Create initial GPU texture (dark)
            uint8_t* mapPx = const_cast<uint8_t*>(mapScreen->getPixels());
            wi::graphics::TextureDesc texDesc;
            texDesc.width = MAP_TEX_SIZE;
            texDesc.height = MAP_TEX_SIZE;
            texDesc.format = wi::graphics::Format::R8G8B8A8_UNORM;
            texDesc.bind_flags = wi::graphics::BindFlag::SHADER_RESOURCE;
            texDesc.mip_levels = 1;
            texDesc.array_size = 1;
            wi::graphics::SubresourceData texData;
            texData.data_ptr = mapPx;
            texData.row_pitch = MAP_TEX_SIZE * 4;
            texData.slice_pitch = texData.row_pitch * MAP_TEX_SIZE;

            static wi::graphics::Texture mapGPUTex;
            wi::graphics::GetDevice()->CreateTexture(&texDesc, &texData, &mapGPUTex);

            // Emissive material (self-lit screen)
            mapMaterialEntity = scene.Entity_CreateMaterial("map_screen_mat");
            auto* mapMat = scene.materials.GetComponent(mapMaterialEntity);
            if (mapMat) {
                mapMat->baseColor = DirectX::XMFLOAT4(0, 0, 0, 1);
                mapMat->roughness = 0.2f;
                mapMat->metalness = 0.0f;
                mapMat->SetEmissiveStrength(3.0f);
                mapMat->emissiveColor = DirectX::XMFLOAT4(1, 1, 1, 1);
                mapMat->textures[wi::scene::MaterialComponent::EMISSIVEMAP].resource.SetTexture(mapGPUTex);
                mapMat->SetDoubleSided(true);
                mapMat->CreateRenderData();
            }

            // Tilted quad mesh (same as radar)
            wi::ecs::Entity mapMeshEntity = scene.Entity_CreateMesh("map_screen_mesh");
            auto* mesh = scene.meshes.GetComponent(mapMeshEntity);
            if (mesh) {
                float half = mapMetres * 0.5f;
                float tiltRad = mapScreenTilt * (float)M_PI / 180.0f;
                float cosT = std::cos(tiltRad);
                float sinT = std::sin(tiltRad);

                mesh->vertex_positions.push_back(DirectX::XMFLOAT3(-half, -half * cosT, -half * sinT));
                mesh->vertex_positions.push_back(DirectX::XMFLOAT3( half, -half * cosT, -half * sinT));
                mesh->vertex_positions.push_back(DirectX::XMFLOAT3( half,  half * cosT,  half * sinT));
                mesh->vertex_positions.push_back(DirectX::XMFLOAT3(-half,  half * cosT,  half * sinT));

                DirectX::XMFLOAT3 normal(0, sinT, -cosT);
                for (int n = 0; n < 4; n++)
                    mesh->vertex_normals.push_back(normal);

                mesh->vertex_uvset_0.push_back(DirectX::XMFLOAT2(0, 1));
                mesh->vertex_uvset_0.push_back(DirectX::XMFLOAT2(1, 1));
                mesh->vertex_uvset_0.push_back(DirectX::XMFLOAT2(1, 0));
                mesh->vertex_uvset_0.push_back(DirectX::XMFLOAT2(0, 0));

                mesh->indices = {0, 1, 2, 0, 2, 3};

                mesh->subsets.push_back(wi::scene::MeshComponent::MeshSubset());
                mesh->subsets.back().materialID = mapMaterialEntity;
                mesh->subsets.back().indexOffset = 0;
                mesh->subsets.back().indexCount = 6;
                mesh->CreateRenderData();
            }

            mapScreenEntity = wi::ecs::CreateEntity();
            scene.transforms.Create(mapScreenEntity);
            scene.names.Create(mapScreenEntity) = "MapScreen";

            wi::ecs::Entity mapObj = scene.Entity_CreateObject("map_screen_obj");
            scene.Component_Attach(mapObj, mapScreenEntity);
            auto* obj = scene.objects.GetComponent(mapObj);
            if (obj) obj->meshID = mapMeshEntity;

            weLog("  Map screen at (" + std::to_string(msx) + "," +
                  std::to_string(msy) + "," + std::to_string(msz) +
                  ") size=" + std::to_string(mapScreenSizeVal) +
                  " zoom=" + std::to_string(mapZoom));
            }
        }

        // Load own ship navigation lights from boat.ini
        {
            // Try ownship boat.ini first, then fall back to Othership boat.ini
            std::string lightIni = boatIni;
            uint32_t numLights = IniFile::iniFileTou32(lightIni, "NumberOfLights");
            if (numLights == 0) {
                std::string otherPath = resolveModelPath("Models/Othership/" + shipName + "/",
                                                          userFolder, "");
                std::string otherBoatIni = otherPath + "boat.ini";
                uint32_t otherLights = IniFile::iniFileTou32(otherBoatIni, "NumberOfLights");
                if (otherLights > 0) {
                    lightIni = otherBoatIni;
                    numLights = otherLights;
                    weLog("  Own ship: using Othership light defs (" + std::to_string(numLights) + ")");
                }
            }

            // Helper lambda to create a nav light entity and push to navLights
            auto addOwnLight = [&](float lx, float ly, float lz,
                                   float r, float g, float b,
                                   float sa, float ea, float rangeNM,
                                   const std::string& seq, float tOff, int idx) {
                WENavLight nlt;
                nlt.shipIndex = -1;
                nlt.localX = lx; nlt.localY = ly; nlt.localZ = lz;
                nlt.r = r; nlt.g = g; nlt.b = b;
                nlt.startAngle = sa; nlt.endAngle = ea;
                while (nlt.startAngle < 0) { nlt.startAngle += 360; nlt.endAngle += 360; }
                nlt.range = rangeNM * (float)M_IN_NM;
                nlt.intensity_cd = colregCandela(rangeNM);
                nlt.sequence = seq;
                nlt.charTime = 0.25f;
                nlt.timeOffset = tOff;
                std::string lightName = "OwnNavLight_" + std::to_string(idx);
                nlt.entity = scene.Entity_CreateLight(lightName);
                auto* lc = scene.lights.GetComponent(nlt.entity);
                if (lc) {
                    lc->SetType(wi::scene::LightComponent::POINT);
                    lc->color = DirectX::XMFLOAT3(r, g, b);
                    lc->intensity = 0.0f;
                    lc->range = nlt.range; // WE uses range for frustum culling AABB
                    lc->SetCastShadow(true); // shadows prevent light bleeding through hull into bridge
                    lc->SetVolumetricsEnabled(false);
                }
                attachLensFlare(scene, nlt.entity, nlt.r, nlt.g, nlt.b);
                navLights.push_back(nlt);
            };

            if (numLights > 0) {
                for (uint32_t nl = 1; nl <= numLights; nl++) {
                    float lx = IniFile::iniFileTof32(lightIni, IniFile::enumerate1("LightX", nl));
                    float ly = IniFile::iniFileTof32(lightIni, IniFile::enumerate1("LightY", nl));
                    float lz = IniFile::iniFileTof32(lightIni, IniFile::enumerate1("LightZ", nl));
                    float lr = IniFile::iniFileTof32(lightIni, IniFile::enumerate1("LightRed", nl)) / 255.0f;
                    float lg = IniFile::iniFileTof32(lightIni, IniFile::enumerate1("LightGreen", nl)) / 255.0f;
                    float lb = IniFile::iniFileTof32(lightIni, IniFile::enumerate1("LightBlue", nl)) / 255.0f;
                    float sa = IniFile::iniFileTof32(lightIni, IniFile::enumerate1("LightStartAngle", nl));
                    float ea = IniFile::iniFileTof32(lightIni, IniFile::enumerate1("LightEndAngle", nl));
                    float rng = IniFile::iniFileTof32(lightIni, IniFile::enumerate1("LightRange", nl));
                    std::string seq = IniFile::iniFileToString(lightIni, IniFile::enumerate1("Sequence", nl));
                    uint32_t ps = IniFile::iniFileTou32(lightIni, IniFile::enumerate1("PhaseStart", nl));
                    float tOff = (ps == 0) ? (60.0f * ((float)rand() / RAND_MAX)) : ((ps - 1) * 0.25f);
                    addOwnLight(lx, ly, lz, lr, lg, lb, sa, ea, rng, seq, tOff, (int)nl);
                }
                weLog("  Own ship: " + std::to_string(numLights) + " nav lights");
            } else if (viewLocalY > 0) {
                // Generate default COLREG lights from bridge view position
                float vy = viewLocalY, vz = viewLocalZ;
                if (vz < 1.0f) vz = vy; // fallback if no Z view data
                //                     X              Y          Z      R G B  StartAngle EndAngle Range
                addOwnLight( vy*0.3f,  vy*0.95f, vz*0.7f,  0,1,0, 359,112.5f, 3, "", 0, 1); // green stbd
                addOwnLight(-vy*0.3f,  vy*0.95f, vz*0.7f,  1,0,0, 247.5f,361, 3, "", 0, 2); // red port
                addOwnLight( 0,        vy*1.3f,  vz*0.8f,  1,1,1, 247.5f,472.5f, 5, "", 0, 3); // masthead fwd
                addOwnLight( 0,        vy*0.5f, -vz*0.8f,  1,1,1, 112.5f,247.5f, 3, "", 0, 4); // stern
                addOwnLight( 0,        vy*1.2f,  vz,       1,1,1, 247.5f,472.5f, 5, "", 0, 5); // masthead aft
                weLog("  Own ship: generated 5 default COLREG nav lights (viewY=" +
                      std::to_string(vy) + " viewZ=" + std::to_string(vz) + ")");
            }
        }
    }

    // ===== OTHER SHIPS =====
    pumpMessages();
    weLog("  Setting up " + std::to_string(scenarioData.otherShipsData.size()) + " other ships...");
    otherShipStates.resize(scenarioData.otherShipsData.size());
    for (size_t s = 0; s < scenarioData.otherShipsData.size(); s++) {
        const OtherShipData& shipData = scenarioData.otherShipsData[s];
        std::string shipName = shipData.shipName;

        // Check Othership folder first, then Ownship
        std::string basePath = "Models/Othership/" + shipName + "/";
        if (!Utilities::pathExists(basePath) && !Utilities::pathExists(userFolder + basePath)) {
            basePath = "Models/Ownship/" + shipName + "/";
        }
        basePath = resolveModelPath(basePath, userFolder, "");

        std::string boatIni = basePath + "boat.ini";
        std::string modelFileName = IniFile::iniFileToString(boatIni, "FileName");
        float scaleFactor = IniFile::iniFileTof32(boatIni, "ScaleFactor");
        if (scaleFactor <= 0) scaleFactor = 1.0f;
        float yCorrection = IniFile::iniFileTof32(boatIni, "YCorrection");
        float heightCorrection = yCorrection * scaleFactor;

        float shipX = coords.longToX(shipData.initialLong);
        float shipZ = coords.latToZ(shipData.initialLat);
        float heading = 0;
        float speed = 0;
        if (!shipData.legs.empty()) {
            heading = shipData.legs[0].bearing;
            speed = shipData.legs[0].speed;
        }

        std::string modelPath = basePath + modelFileName;
        std::string objName = "OtherShip_" + std::to_string(s);
        wi::ecs::Entity shipEntity = loadModelOrPlaceholder(scene, modelPath,
                                                             objName, 0.6f, 0.6f, 0.6f, 15.0f,
                                                             true /*allowTransparency: ship windows*/);
        setEntityTransform(scene, shipEntity, shipX, heightCorrection, shipZ,
                           heading, scaleFactor);

        // Store state for movement simulation
        otherShipStates[s].entity = shipEntity;
        otherShipStates[s].x = shipX;
        otherShipStates[s].z = shipZ;
        otherShipStates[s].heading = heading;
        otherShipStates[s].speed = speed;
        otherShipStates[s].heightCorr = heightCorrection;
        otherShipStates[s].scaleFactor = scaleFactor;
        otherShipStates[s].currentLeg = 0;
        otherShipStates[s].distTravelled = 0;

        weLog("  Other ship " + std::to_string(s) + ": " + shipName +
              " speed=" + std::to_string(speed) + "kn heading=" + std::to_string(heading));

        // Load navigation lights from boat.ini
        uint32_t numLights = IniFile::iniFileTou32(boatIni, "NumberOfLights");
        for (uint32_t nl = 1; nl <= numLights; nl++) {
            WENavLight nlt;
            nlt.shipIndex = (int)s;
            nlt.localX = IniFile::iniFileTof32(boatIni, IniFile::enumerate1("LightX", nl));
            nlt.localY = IniFile::iniFileTof32(boatIni, IniFile::enumerate1("LightY", nl));
            nlt.localZ = IniFile::iniFileTof32(boatIni, IniFile::enumerate1("LightZ", nl));

            float lightR = IniFile::iniFileTof32(boatIni, IniFile::enumerate1("LightRed", nl)) / 255.0f;
            float lightG = IniFile::iniFileTof32(boatIni, IniFile::enumerate1("LightGreen", nl)) / 255.0f;
            float lightB = IniFile::iniFileTof32(boatIni, IniFile::enumerate1("LightBlue", nl)) / 255.0f;
            nlt.r = lightR; nlt.g = lightG; nlt.b = lightB;

            nlt.startAngle = IniFile::iniFileTof32(boatIni, IniFile::enumerate1("LightStartAngle", nl));
            nlt.endAngle = IniFile::iniFileTof32(boatIni, IniFile::enumerate1("LightEndAngle", nl));
            // Fix negative start angles (same as NavLight.cpp)
            while (nlt.startAngle < 0) { nlt.startAngle += 360; nlt.endAngle += 360; }

            float rangeNM = IniFile::iniFileTof32(boatIni, IniFile::enumerate1("LightRange", nl));
            if (rangeNM <= 0) rangeNM = 3.0f; // default 3nm if not specified
            nlt.range = rangeNM * (float)M_IN_NM; // Nm -> metres
            nlt.intensity_cd = colregCandela(rangeNM);

            nlt.sequence = IniFile::iniFileToString(boatIni, IniFile::enumerate1("Sequence", nl));
            uint32_t phaseStart = IniFile::iniFileTou32(boatIni, IniFile::enumerate1("PhaseStart", nl));
            nlt.charTime = 0.25f;
            nlt.timeOffset = (phaseStart == 0) ? (60.0f * ((float)rand() / RAND_MAX)) : ((phaseStart - 1) * nlt.charTime);

            std::string lightName = "NavLight_" + std::to_string(s) + "_" + std::to_string(nl);
            nlt.entity = scene.Entity_CreateLight(lightName);
            auto* lightComp = scene.lights.GetComponent(nlt.entity);
            if (lightComp) {
                lightComp->SetType(wi::scene::LightComponent::POINT);
                lightComp->color = DirectX::XMFLOAT3(lightR, lightG, lightB);
                lightComp->intensity = 0.0f; // update loop controls
                lightComp->range = nlt.range; // WE uses range for frustum culling AABB -- must match visibility distance
                lightComp->SetCastShadow(false);
                lightComp->SetVolumetricsEnabled(false);
            }
            attachLensFlare(scene, nlt.entity, nlt.r, nlt.g, nlt.b);

            navLights.push_back(nlt);
        }
        if (numLights > 0)
            weLog("    " + std::to_string(numLights) + " nav lights");
    }

    // ===== BUOYS =====
    pumpMessages();
    weLog("  Setting up buoys...");
    std::string buoyIniFile = worldPath + "buoy.ini";
    if (Utilities::pathExists(buoyIniFile)) {
        uint32_t numBuoys = IniFile::iniFileTou32(buoyIniFile, "Number");
        weLog("  Found " + std::to_string(numBuoys) + " buoys in buoy.ini");
        buoyStates.resize(numBuoys);
        for (uint32_t b = 1; b <= numBuoys; b++) {
            std::string buoyType = IniFile::iniFileToString(buoyIniFile, IniFile::enumerate1("Type", b));
            float buoyLon = IniFile::iniFileTof32(buoyIniFile, IniFile::enumerate1("Long", b));
            float buoyLat = IniFile::iniFileTof32(buoyIniFile, IniFile::enumerate1("Lat", b));
            float heightCorr = IniFile::iniFileTof32(buoyIniFile, IniFile::enumerate1("HeightCorrection", b));

            float bx = coords.longToX(buoyLon);
            float bz = coords.latToZ(buoyLat);

            // Load buoy model
            std::string buoyBasePath = resolveModelPath("Models/Buoy/" + buoyType + "/",
                                                         userFolder, worldPath);
            std::string buoyModelIni = buoyBasePath + "buoy.ini";
            std::string buoyFileName = IniFile::iniFileToString(buoyModelIni, "FileName", "buoy.x");
            float buoyScale = IniFile::iniFileTof32(buoyModelIni, "Scalefactor", 1.f);

            std::string buoyModelPath = buoyBasePath + buoyFileName;
            std::string objName = "Buoy_" + std::to_string(b - 1);

            // Read colour override from buoy.ini (new field, backward compatible)
            std::string buoyColours = IniFile::iniFileToString(buoyIniFile,
                IniFile::enumerate1("Colours", b));

            wi::ecs::Entity buoyEntity;
            if (!buoyColours.empty()) {
                buoyEntity = loadBuoyWithColour(scene, buoyModelPath, objName, buoyColours);
            } else {
                buoyEntity = loadModelOrPlaceholder(scene, buoyModelPath,
                                                     objName, 0.8f, 0.2f, 0.2f, 3.0f);
            }
            setEntityTransform(scene, buoyEntity, bx, heightCorr, bz, 0, buoyScale);

            buoyStates[b - 1].entity = buoyEntity;
            buoyStates[b - 1].heightCorr = heightCorr;
            buoyStates[b - 1].scaleFactor = buoyScale;
        }
        weLog("  Loaded " + std::to_string(numBuoys) + " buoys");
    }

    // ===== LAND OBJECTS =====
    pumpMessages();
    weLog("  Setting up land objects...");
    std::string landObjIniFile = worldPath + "landobject.ini";
    if (Utilities::pathExists(landObjIniFile)) {
        uint32_t numLandObjs = IniFile::iniFileTou32(landObjIniFile, "Number");
        weLog("  Found " + std::to_string(numLandObjs) + " land objects");
        for (uint32_t lo = 1; lo <= numLandObjs; lo++) {
            std::string objType = IniFile::iniFileToString(landObjIniFile, IniFile::enumerate1("Type", lo));
            float objLon = IniFile::iniFileTof32(landObjIniFile, IniFile::enumerate1("Long", lo));
            float objLat = IniFile::iniFileTof32(landObjIniFile, IniFile::enumerate1("Lat", lo));
            float objY = IniFile::iniFileTof32(landObjIniFile, IniFile::enumerate1("HeightCorrection", lo));
            float rotation = IniFile::iniFileTof32(landObjIniFile, IniFile::enumerate1("Rotation", lo));

            float ox = coords.longToX(objLon);
            float oz = coords.latToZ(objLat);

            // Place at terrain surface height + correction (BC LandObjects.cpp logic)
            // Absolute=0: relative to terrain, Absolute=1: absolute Y, Absolute=2: relative but clamped to sea level
            uint32_t absolute = IniFile::iniFileTou32(landObjIniFile, IniFile::enumerate1("Absolute", lo));
            if (absolute == 0 && terrainNode) {
                objY += terrainNode->getHeightAt(ox, oz) + terrainNode->getPosition().y;
            } else if (absolute == 2 && terrainNode) {
                objY += std::max(0.0f, terrainNode->getHeightAt(ox, oz) + terrainNode->getPosition().y);
            }

            // Load land object model
            std::string loBasePath = resolveModelPath("Models/LandObject/" + objType + "/",
                                                       userFolder, worldPath);
            std::string loModelIni = loBasePath + "object.ini";
            std::string loFileName = IniFile::iniFileToString(loModelIni, "FileName", "object.x");
            float loScale = IniFile::iniFileTof32(loModelIni, "Scalefactor", 1.f);

            std::string loModelPath = loBasePath + loFileName;
            weLog("  LandObject " + std::to_string(lo) + "/" + std::to_string(numLandObjs) +
                  ": " + objType + " model=" + loModelPath);
            std::string objName = "LandObject_" + std::to_string(lo - 1);
            wi::ecs::Entity loEntity;
            if (objType == "Lighthouse") {
                // Use procedural lighthouse (GLB import via temp scene merge
                // produces invisible geometry -- needs further investigation)
                float lhHeight = IniFile::iniFileTof32(landObjIniFile, IniFile::enumerate1("HeightAbove", lo));
                if (lhHeight < 1.0f) lhHeight = 15.0f;
                loEntity = createProceduralLighthouse(scene, objName, lhHeight);
                loScale = 1.0f;
            } else {
                loEntity = loadModelOrPlaceholder(scene, loModelPath,
                                                   objName, 0.5f, 0.35f, 0.2f, 8.0f);
            }
            setEntityTransform(scene, loEntity, ox, objY, oz, rotation, loScale);
        }
        weLog("  Loaded " + std::to_string(numLandObjs) + " land objects");
    }

    // ===== BUOY & LAND LIGHTS (from light.ini) =====
    pumpMessages();
    weLog("  Setting up navigation lights from light.ini...");
    {
        std::string lightIniFile = worldPath + "light.ini";
        if (Utilities::pathExists(lightIniFile)) {
            uint32_t numSceneLights = IniFile::iniFileTou32(lightIniFile, "Number");
            weLog("  Found " + std::to_string(numSceneLights) + " lights in light.ini");
            int createdCount = 0;

            for (uint32_t li = 1; li <= numSceneLights; li++) {
                uint32_t buoyRef = IniFile::iniFileTou32(lightIniFile, IniFile::enumerate1("Buoy", li));
                float lightR = IniFile::iniFileTof32(lightIniFile, IniFile::enumerate1("Red", li)) / 255.0f;
                float lightG = IniFile::iniFileTof32(lightIniFile, IniFile::enumerate1("Green", li)) / 255.0f;
                float lightB = IniFile::iniFileTof32(lightIniFile, IniFile::enumerate1("Blue", li)) / 255.0f;
                float lightRange = IniFile::iniFileTof32(lightIniFile, IniFile::enumerate1("Range", li));
                if (lightRange <= 0) lightRange = 5.0f;
                float lightHeight = IniFile::iniFileTof32(lightIniFile, IniFile::enumerate1("Height", li));
                if (lightHeight <= 0) lightHeight = 5.0f;
                uint32_t absolute = IniFile::iniFileTou32(lightIniFile, IniFile::enumerate1("Absolute", li));
                float startAngle = IniFile::iniFileTof32(lightIniFile, IniFile::enumerate1("StartAngle", li));
                float endAngle = IniFile::iniFileTof32(lightIniFile, IniFile::enumerate1("EndAngle", li));
                if (startAngle == 0 && endAngle == 0) endAngle = 360.0f;
                std::string sequence = IniFile::iniFileToString(lightIniFile, IniFile::enumerate1("Sequence", li));
                uint32_t phaseStart = IniFile::iniFileTou32(lightIniFile, IniFile::enumerate1("PhaseStart", li));
                if (phaseStart < 1) phaseStart = 1;

                WENavLight nlt;
                nlt.r = lightR;
                nlt.g = lightG;
                nlt.b = lightB;
                nlt.startAngle = startAngle;
                nlt.endAngle = endAngle;
                nlt.range = lightRange * 1852.0f; // nm to metres
                nlt.intensity_cd = colregCandela(lightRange);
                nlt.sequence = sequence;
                nlt.charTime = 0.25f; // standard quarter-second per character
                nlt.timeOffset = (float)(phaseStart - 1) * nlt.charTime;

                if (buoyRef > 0 && buoyRef <= (uint32_t)buoyStates.size()) {
                    // Buoy-attached light: position resolved per-frame from buoyStates
                    nlt.shipIndex = -2;
                    nlt.buoyIndex = (int)(buoyRef - 1); // light.ini is 1-based
                    nlt.localY = lightHeight; // height above buoy base
                } else {
                    // Fixed land light (lighthouse, shore mark, etc.)
                    float lLon = IniFile::iniFileTof32(lightIniFile, IniFile::enumerate1("Long", li));
                    float lLat = IniFile::iniFileTof32(lightIniFile, IniFile::enumerate1("Lat", li));
                    if (lLon == 0 && lLat == 0 && buoyRef > 0) continue; // invalid buoy ref, skip

                    float lx = coords.longToX(lLon);
                    float lz = coords.latToZ(lLat);
                    float ly = lightHeight;
                    if (absolute == 0 && terrainNode) {
                        ly += terrainNode->getHeightAt(lx, lz) + terrainNode->getPosition().y;
                    } else if (absolute == 2 && terrainNode) {
                        ly += std::max(0.0f, terrainNode->getHeightAt(lx, lz) + terrainNode->getPosition().y);
                    }
                    // absolute == 1: ly = lightHeight (absolute above sea level)

                    nlt.shipIndex = -2;
                    nlt.buoyIndex = -1;
                    nlt.localX = lx;  // world X (repurposed for fixed lights)
                    nlt.localY = ly;  // world Y
                    nlt.localZ = lz;  // world Z
                }

                // Create WE point light entity
                std::string lightName = "SceneLight_" + std::to_string(li);
                nlt.entity = scene.Entity_CreateLight(lightName);
                auto* lightComp = scene.lights.GetComponent(nlt.entity);
                if (lightComp) {
                    lightComp->SetType(wi::scene::LightComponent::POINT);
                    lightComp->color = XMFLOAT3(nlt.r, nlt.g, nlt.b);
                    lightComp->intensity = 0.0f;
                    lightComp->range = nlt.range; // WE uses range for frustum culling AABB -- must match visibility distance
                    lightComp->SetCastShadow(false);
                    lightComp->SetVolumetricsEnabled(false);
                }
                attachLensFlare(scene, nlt.entity, nlt.r, nlt.g, nlt.b);

                navLights.push_back(nlt);
                createdCount++;

                weLog("    Light " + std::to_string(li) + ": pos=(" +
                      std::to_string(nlt.localX) + "," + std::to_string(nlt.localY) + "," +
                      std::to_string(nlt.localZ) + ") arc=" + std::to_string(nlt.startAngle) +
                      "-" + std::to_string(nlt.endAngle) + " rgb=(" +
                      std::to_string(nlt.r) + "," + std::to_string(nlt.g) + "," +
                      std::to_string(nlt.b) + ") buoy=" + std::to_string(nlt.buoyIndex));
            }
            weLog("  Created " + std::to_string(createdCount) + " scene lights (buoy + land)");
        }
    }

    // ===== AUTO-GENERATE BUOY LIGHTS (for buoys without light.ini entries) =====
    {
        // Find which buoys already have lights from light.ini
        std::vector<bool> buoyHasLight(buoyStates.size(), false);
        for (const auto& nlt : navLights) {
            if (nlt.buoyIndex >= 0 && nlt.buoyIndex < (int)buoyHasLight.size())
                buoyHasLight[nlt.buoyIndex] = true;
        }
        int autoLightCount = 0;
        for (uint32_t b = 0; b < buoyStates.size(); b++) {
            if (buoyHasLight[b]) continue;
            std::string buoyIniFile = worldPath + "buoy.ini";
            std::string colours = IniFile::iniFileToString(buoyIniFile, IniFile::enumerate1("Colours", b + 1));
            std::string colourPattern = IniFile::iniFileToString(buoyIniFile, IniFile::enumerate1("ColourPattern", b + 1));
            if (colours.empty()) continue; // no colour info, skip

            // Determine light colour and sequence from buoy colours
            float lr = 1, lg = 1, lb = 1;
            std::string seq = "LLLLDDDDDDDDDDDD"; // default Fl 4s
            bool isCardinal = false;

            if (colours.find("green") != std::string::npos && colours.find("red") == std::string::npos) {
                lr = 0; lg = 1; lb = 0; // green
            } else if (colours.find("red") != std::string::npos && colours.find("green") == std::string::npos) {
                lr = 1; lg = 0; lb = 0; // red
            } else if (colourPattern == "horizontal" &&
                       (colours.find("yellow") != std::string::npos || colours.find("black") != std::string::npos)) {
                // Cardinal mark: white light with Q or VQ pattern
                lr = 1; lg = 1; lb = 1;
                isCardinal = true;
                // Determine cardinal direction from colour pattern
                if (colours.find("black;yellow") == 0) {
                    // North cardinal: black over yellow → Q continuous
                    seq = "LDLDLDLDLDLDLDLD";
                } else if (colours.find("yellow;black;yellow") == 0) {
                    // West cardinal: yellow/black/yellow → VQ(9) 10s
                    seq = "LDLDLDLDLDLDLDLDLDDDDDDDDDDDDDDDDDDDDD";
                } else if (colours.find("yellow;black") == 0) {
                    // South cardinal: yellow over black → VQ(6)+LFl 10s
                    seq = "LDLDLDLDLDLDDDDDDDDDLLLLLLDDDDDDDDDDDD";
                } else if (colours.find("black;yellow;black") == 0) {
                    // East cardinal: black/yellow/black → VQ(3) 5s
                    seq = "LDLDLDDDDDDDDDDDDDDD";
                } else {
                    seq = "LDLDLDLDLDLDLDLD"; // default Q
                }
            }

            WENavLight nlt;
            nlt.shipIndex = -2;
            nlt.buoyIndex = (int)b;
            nlt.localY = 3.0f; // light 3m above buoy base
            nlt.r = lr; nlt.g = lg; nlt.b = lb;
            nlt.startAngle = 0; nlt.endAngle = 360; // all-round
            float buoyRangeNM = isCardinal ? 5.0f : 3.0f;
            nlt.range = buoyRangeNM * 1852.0f; // 3-5 nm
            nlt.intensity_cd = colregCandela(buoyRangeNM);
            nlt.sequence = seq;
            nlt.charTime = 0.25f;
            nlt.timeOffset = (float)(b % 20) * 0.25f; // stagger phase

            std::string lightName = "BuoyAutoLight_" + std::to_string(b);
            nlt.entity = scene.Entity_CreateLight(lightName);
            auto* lightComp = scene.lights.GetComponent(nlt.entity);
            if (lightComp) {
                lightComp->SetType(wi::scene::LightComponent::POINT);
                lightComp->color = XMFLOAT3(lr, lg, lb);
                lightComp->intensity = 0.0f;
                lightComp->range = nlt.range; // WE uses range for frustum culling AABB -- must match visibility distance
                lightComp->SetCastShadow(false);
                lightComp->SetVolumetricsEnabled(false);
            }
            attachLensFlare(scene, nlt.entity, nlt.r, nlt.g, nlt.b);
            navLights.push_back(nlt);
            autoLightCount++;
        }
        if (autoLightCount > 0)
            weLog("  Auto-generated " + std::to_string(autoLightCount) + " buoy lights");
    }

    // ===== OSM BUILDINGS (procedural or pre-baked) =====
    pumpMessages();
    bool hasPrebaked = false;
    {
        std::string prebakePath = worldPath + "buildings.obj";
        std::ifstream test(prebakePath);
        hasPrebaked = test.good();
    }
    if (hasPrebaked) {
        // Load pre-baked buildings.obj directly (world-space coordinates, no transform needed)
        std::string prebakePath = worldPath + "buildings.obj";
        weLog("  Loading pre-baked buildings: " + prebakePath);
        wi::ecs::Entity bldgEntity = loadModelOrPlaceholder(scene, prebakePath,
                                                             "PrebakeBuildings", 0.5f, 0.45f, 0.35f, 8.0f);
        if (bldgEntity != wi::ecs::INVALID_ENTITY) {
            // Set double-sided + shadow casting on building materials
            // (OBJ .mtl format has no double-sided flag, must set in code)
            for (size_t i = 0; i < scene.materials.GetCount(); i++) {
                auto* nameComp = scene.names.GetComponent(scene.materials.GetEntity(i));
                if (!nameComp) continue;
                const auto& n = nameComp->name;
                if (n.find("building") != std::string::npos ||
                    n.find("Building") != std::string::npos ||
                    n == "BC_DefaultMaterial") {
                    scene.materials[i].SetDoubleSided(true);
                    scene.materials[i].SetCastShadow(true);
                }
            }
            weLog("  Pre-baked buildings loaded");
        }

        // Load pre-baked structures (dams, breakwaters, piers) with concrete material
        std::string structPath = worldPath + "structures.obj";
        if (Utilities::pathExists(structPath)) {
            weLog("  Loading pre-baked structures: " + structPath);
            wi::ecs::Entity structEntity = loadModelOrPlaceholder(scene, structPath,
                                                                    "PrebakeStructures", 0.7f, 0.7f, 0.68f, 8.0f);
            if (structEntity != wi::ecs::INVALID_ENTITY) {
                // Override all materials to concrete (grey, rough, double-sided)
                for (size_t i = 0; i < scene.materials.GetCount(); i++) {
                    auto* nameComp = scene.names.GetComponent(scene.materials.GetEntity(i));
                    if (!nameComp) continue;
                    const auto& n = nameComp->name;
                    if (n.find("concrete") != std::string::npos) {
                        scene.materials[i].baseColor = DirectX::XMFLOAT4(0.85f, 0.85f, 0.82f, 1.0f);
                        scene.materials[i].roughness = 0.80f;
                        scene.materials[i].metalness = 0.0f;
                        scene.materials[i].SetDoubleSided(true);
                        scene.materials[i].SetCastShadow(true);
                        // Keep the concrete texture if loaded from MTL
                        scene.materials[i].CreateRenderData();
                    }
                }
                weLog("  Pre-baked structures loaded");
            }
        }
    } else if (coords.terrainLongExtent > 0 && coords.terrainLatExtent > 0) {
        weLog("  Loading OSM buildings...");

        double minLat = coords.terrainLat;
        double maxLat = coords.terrainLat + coords.terrainLatExtent;
        double minLon = coords.terrainLong;
        double maxLon = coords.terrainLong + coords.terrainLongExtent;

        std::string cacheFile = worldPath + "buildings_cache.dat";
        OSMBuildingReader bldgReader;
        bool haveBuildings = false;

        // Try loading from cache first
        if (bldgReader.loadCache(cacheFile)) {
            weLog("  Loaded " + std::to_string(bldgReader.getBuildings().size()) +
                  " buildings from cache");
            haveBuildings = true;
        } else {
            // Query Overpass API (requires internet)
            bool queryOk = bldgReader.query(minLat, maxLat, minLon, maxLon,
                                             [](const std::string& msg) {
                                                 weLog("  OSM: " + msg);
                                             });
            if (queryOk && !bldgReader.getBuildings().empty()) {
                haveBuildings = true;
                // Cache for next time
                if (bldgReader.saveCache(cacheFile)) {
                    weLog("  Cached building data to " + cacheFile);
                }
            } else if (!queryOk) {
                weLog("  OSM building query failed: " + bldgReader.getError());
            }
        }

        if (haveBuildings && !bldgReader.getBuildings().empty()) {
            const auto& allFootprints = bldgReader.getBuildings();

            auto coordFunc = [&](double lat, double lon) -> std::pair<float, float> {
                float x = coords.longToX(static_cast<float>(lon));
                float z = coords.latToZ(static_cast<float>(lat));
                return {x, z};
            };

            // Sort buildings by distance to own ship so nearest are generated first
            struct ScoredFP {
                const BuildingFootprint* fp;
                float distSq; // squared distance to camera in world coords
            };
            std::vector<ScoredFP> scored;
            scored.reserve(allFootprints.size());
            for (const auto& fp : allFootprints) {
                if (fp.outline.size() < 3) continue;
                double centLat = 0, centLon = 0;
                for (const auto& [lat, lon] : fp.outline) {
                    centLat += lat; centLon += lon;
                }
                centLat /= fp.outline.size();
                centLon /= fp.outline.size();
                float cx = coords.longToX(static_cast<float>(centLon));
                float cz = coords.latToZ(static_cast<float>(centLat));
                float dx = cx - ownShipX;
                float dz = cz - ownShipZ;
                scored.push_back({&fp, dx * dx + dz * dz});
            }
            std::sort(scored.begin(), scored.end(),
                      [](const ScoredFP& a, const ScoredFP& b) { return a.distSq < b.distSq; });

            // Limit: max 5000 buildings / 200K vertices to keep GPU happy
            static const size_t MAX_BUILDINGS = 5000;
            static const size_t MAX_VERTICES = 200000;
            static const size_t VERTS_PER_TILE = 50000; // split into multiple mesh entities

            float terrainPosY = (terrainNode) ? terrainNode->getPosition().y : 0.0f;

            // Check for wall and roof textures in world directory
            std::string wallTexPath, roofTexPath;
            {
                std::string candidate = worldPath + "building_wall.png";
                std::ifstream test(candidate);
                if (test.good()) wallTexPath = candidate;
                else {
                    candidate = worldPath + "building_facade.png";
                    std::ifstream test2(candidate);
                    if (test2.good()) wallTexPath = candidate;
                }
            }
            {
                std::string candidate = worldPath + "building_roof.png";
                std::ifstream test(candidate);
                if (test.good()) roofTexPath = candidate;
            }
            if (!wallTexPath.empty()) weLog("  Wall texture: " + wallTexPath);
            if (!roofTexPath.empty()) weLog("  Roof texture: " + roofTexPath);

            BuildingMesh buildingBatch, structureBatch;
            int totalBuildings = 0, totalStructures = 0, skippedWater = 0;
            int bldgTileIdx = 0, structTileIdx = 0;
            size_t totalVerts = 0;

            for (size_t si = 0; si < scored.size() && (totalBuildings + totalStructures) < (int)MAX_BUILDINGS; si++) {
                const auto& fp = *scored[si].fp;

                // Compute centroid for terrain height
                double centLat = 0, centLon = 0;
                for (const auto& [lat, lon] : fp.outline) {
                    centLat += lat; centLon += lon;
                }
                centLat /= fp.outline.size();
                centLon /= fp.outline.size();
                float cx = coords.longToX(static_cast<float>(centLon));
                float cz = coords.latToZ(static_cast<float>(centLat));

                float groundY = 0.0f;
                if (terrainNode) {
                    groundY = terrainNode->getHeightAt(cx, cz) + terrainPosY;
                }

                if (groundY < -0.5f && !fp.isStructure) { skippedWater++; continue; }
                if (groundY < 0.0f) groundY = 0.0f;

                BuildingMesh single = BuildingGenerator::generate(fp, coordFunc, groundY);
                if (single.empty()) continue;

                totalVerts += single.vertexCount();

                if (fp.isStructure) {
                    structureBatch.append(single);
                    totalStructures++;
                    if (structureBatch.vertexCount() >= VERTS_PER_TILE) {
                        wi::ecs::Entity e = createBuildingMeshEntity(
                            scene, structureBatch, "OSM_Structures_" + std::to_string(structTileIdx),
                            "", "", true);
                        if (e != wi::ecs::INVALID_ENTITY)
                            setEntityTransform(scene, e, 0, 0, 0);
                        structureBatch = BuildingMesh();
                        structTileIdx++;
                    }
                } else {
                    buildingBatch.append(single);
                    totalBuildings++;
                    if (buildingBatch.vertexCount() >= VERTS_PER_TILE || totalVerts >= MAX_VERTICES) {
                        wi::ecs::Entity e = createBuildingMeshEntity(
                            scene, buildingBatch, "OSM_Buildings_" + std::to_string(bldgTileIdx),
                            wallTexPath, roofTexPath, false);
                        if (e != wi::ecs::INVALID_ENTITY)
                            setEntityTransform(scene, e, 0, 0, 0);
                        buildingBatch = BuildingMesh();
                        bldgTileIdx++;
                        if (totalVerts >= MAX_VERTICES) break;
                    }
                }
            }

            // Flush remaining building batch
            if (!buildingBatch.empty()) {
                wi::ecs::Entity e = createBuildingMeshEntity(
                    scene, buildingBatch, "OSM_Buildings_" + std::to_string(bldgTileIdx),
                    wallTexPath, roofTexPath, false);
                if (e != wi::ecs::INVALID_ENTITY)
                    setEntityTransform(scene, e, 0, 0, 0);
                bldgTileIdx++;
            }
            // Flush remaining structure batch
            if (!structureBatch.empty()) {
                wi::ecs::Entity e = createBuildingMeshEntity(
                    scene, structureBatch, "OSM_Structures_" + std::to_string(structTileIdx),
                    "", "", true);
                if (e != wi::ecs::INVALID_ENTITY)
                    setEntityTransform(scene, e, 0, 0, 0);
                structTileIdx++;
            }

            int totalTiles = bldgTileIdx + structTileIdx;
            weLog("  Created " + std::to_string(totalBuildings) + " buildings + " +
                  std::to_string(totalStructures) + " structures in " +
                  std::to_string(totalTiles) + " tiles (" +
                  std::to_string(totalVerts) + " verts)");
            if (skippedWater > 0)
                weLog("  Skipped " + std::to_string(skippedWater) + " buildings in water");
        } else if (!haveBuildings) {
            weLog("  No buildings found in this area");
        }
    }

    // ===== SHUTDOWN IRRLICHT CONVERTER =====
    // All models loaded; release the headless Irrlicht device
    bc::shutdownIrrlichtConverter();
    weLog("  Irrlicht model converter shutdown.");

    // ===== CAMERA SETUP =====
    // First-person bridge view: camera AT the bridge position, looking along heading
    camPosX = cameraViewX;
    camPosY = std::max(cameraViewY, 3.0f);
    camPosZ = cameraViewZ; // both BC and WE are left-handed, no Z flip
    camYaw = ownShipHeading;
    camPitch = 0.0f;
    camOrbitMode = false;

    // Orbit mode target (for when 'O' is pressed)
    camTargetX = ownShipX;
    camTargetY = camPosY;
    camTargetZ = ownShipZ;
    camDistance = 200.0f;

    weLog("  Camera bridge pos: (" + std::to_string(camPosX) + ", " +
          std::to_string(camPosY) + ", " + std::to_string(camPosZ) + ")");
    weLog("  Camera heading=" + std::to_string(camYaw) +
          " mode=" + (camOrbitMode ? std::string("orbit") : std::string("bridge")));

    } catch (const std::exception& e) {
        weLogErr("During scene setup: " + std::string(e.what()));
    } catch (...) {
        weLogErr("During scene setup (unknown exception)");
    }

    weLog("  Scene setup complete.");

    // --- Initialize full SimulationModel via headless Irrlicht device ---
    weLog("  Initializing SimulationBridge (physics/AI)...");
    SimBridge::init(&sound, scenarioData);
    SimBridge::start();
    // OwnShip's constructor already sets the engine to the correct proportion
    // for the initial speed (using the quadratic dynamics model). Read it back
    // so our local engine variables match.
    {
        ownShipPortEngine = SimBridge::getPortEngine();
        ownShipStbdEngine = SimBridge::getStbdEngine();
    }
    // Enable ARPA auto-detection so radar contacts can be clicked to track
    SimBridge::setArpaMode(1);

    weLog("  SimulationBridge ready (initial engine: " +
          std::to_string(ownShipPortEngine) + "/" + std::to_string(ownShipStbdEngine) + ")");

    // Re-focus window after long scene setup (may have lost foreground during loading)
    SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetWindowPos(hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetForegroundWindow(hWnd);
    pumpMessages();

    weLog("Entering Wicked Engine render loop...");
    weLog("  Controls: Mouse-drag=look, WASD=move, O=orbit/bridge, Scroll=zoom(orbit)/FOV(bridge), ESC=quit");

    static bool keyOWasDown = false;
    // Bridge view uses 75 horizontal FOV; WE camera.fov is vertical FOV
    // Convert: VFOV = 2 * atan(tan(HFOV/2) / aspect)
    float hfovRad = 75.0f * (float)M_PI / 180.0f;
    float aspect = (float)width / (float)height;
    float cameraFov = 2.0f * std::atan(std::tan(hfovRad / 2.0f) / aspect) * 180.0f / (float)M_PI;

    // Timing for simulation
    auto lastFrameTime = std::chrono::high_resolution_clock::now();
    float totalSimTime = 0.0f;
    static constexpr float KNOTS_TO_MPS = 0.514444f; // 1 knot = 0.514444 m/s
    static constexpr float NM_TO_M = 1852.0f;        // 1 nautical mile = 1852 m

    // Flush Irrlicht timer so first physics frame has a small dt
    // (timer has been running during entire scene setup above)
    SimBridge::syncTimer();

    // Compute absolute paths for screenshot file-watch (CWD may have shifted
    // during Irrlicht model loading, so relative paths are unreliable).
    std::string screenshotRequestPath, screenshotOutputPath;
    {
        char exePath[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        std::string exeDir(exePath);
        auto slashPos = exeDir.find_last_of("\\/");
        if (slashPos != std::string::npos) exeDir = exeDir.substr(0, slashPos);
        screenshotRequestPath = exeDir + "\\screenshot_request.txt";
        screenshotOutputPath = exeDir + "\\screenshot.png";
    }
    weLog("  Screenshot watch: " + screenshotRequestPath);

    MSG msg = {};
    int frameCount = 0;
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        } else {

            // Frame timing
            auto now = std::chrono::high_resolution_clock::now();
            float dt = std::chrono::duration<float>(now - lastFrameTime).count();
            dt = std::min(dt, 0.1f); // clamp to prevent huge jumps
            lastFrameTime = now;
            totalSimTime += dt;

            // Log first few frames to diagnose crash timing
            frameCount++;
            g_frameCountForCrash = frameCount;
            if (frameCount <= 5) {
                weLog("  Frame " + std::to_string(frameCount) +
                      " dt=" + std::to_string(dt) +
                      " cam=(" + std::to_string(camPosX) + "," +
                      std::to_string(camPosY) + "," + std::to_string(camPosZ) + ")" +
                      " active=" + std::to_string(application.is_window_active));
            }

            // ESC = close overlays first, then toggle pause menu (debounced)
            {
                static bool escWasDown = false;
                bool escDown = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
                if (escDown && !escWasDown) {
                    if (showSettings) {
                        showSettings = false;
                    } else if (showEcdisFullscreen) {
                        showEcdisFullscreen = false;
                    } else if (showRadarFullscreen) {
                        showRadarFullscreen = false;
                    } else {
                        showEscMenu = !showEscMenu;
                    }
                }
                escWasDown = escDown;
            }

            // R key toggles full-screen radar (debounced)
            // Closes ECDIS if open (only one fullscreen view at a time)
            {
                static bool rWasDown = false;
                bool rDown = (GetAsyncKeyState('R') & 0x8000) != 0;
                if (rDown && !rWasDown && !showEscMenu && !showSettings) {
                    if (showEcdisFullscreen) {
                        // Switch from ECDIS to radar
                        showEcdisFullscreen = false;
                        showRadarFullscreen = true;
                    } else {
                        showRadarFullscreen = !showRadarFullscreen;
                    }
                }
                rWasDown = rDown;
            }

            // M key toggles full-screen ECDIS (debounced)
            // Closes radar if open (only one fullscreen view at a time)
            {
                static bool mWasDown = false;
                bool mDown = (GetAsyncKeyState('M') & 0x8000) != 0;
                if (mDown && !mWasDown && !showEscMenu && !showSettings) {
                    if (showRadarFullscreen) {
                        // Switch from radar to ECDIS
                        showRadarFullscreen = false;
                        showEcdisFullscreen = true;
                    } else {
                        showEcdisFullscreen = !showEcdisFullscreen;
                    }
                }
                mWasDown = mDown;
            }

            // ===== OWN SHIP CONTROLS =====
            // Skip keyboard controls if ImGui wants input, GUI slider is active, or overlay is open
            bool imguiWantsKB = bc::graphics::wicked::ImGuiWantsKeyboard() || showEscMenu || showSettings || showRadarFullscreen;
            bool guiControlActive = overlay.isControlActive();
            // Arrow Up/Down: engine ahead/astern (telegraph-style, ~5s full travel)
            // Both engines move together via keyboard
            if (!imguiWantsKB && !guiControlActive && GetAsyncKeyState(VK_UP) & 0x8000) {
                ownShipPortEngine = std::min(1.0f, ownShipPortEngine + 0.2f * dt);
                ownShipStbdEngine = std::min(1.0f, ownShipStbdEngine + 0.2f * dt);
            }
            if (!imguiWantsKB && !guiControlActive && GetAsyncKeyState(VK_DOWN) & 0x8000) {
                ownShipPortEngine = std::max(-1.0f, ownShipPortEngine - 0.2f * dt);
                ownShipStbdEngine = std::max(-1.0f, ownShipStbdEngine - 0.2f * dt);
            }
            // Arrow Left/Right: wheel (helm rate ~10 deg/s for responsive feel)
            if (!imguiWantsKB && !guiControlActive && GetAsyncKeyState(VK_LEFT) & 0x8000) {
                ownShipRudder = std::max(-30.0f, ownShipRudder - 10.0f * dt);
            } else if (!imguiWantsKB && !guiControlActive && GetAsyncKeyState(VK_RIGHT) & 0x8000) {
                ownShipRudder = std::min(30.0f, ownShipRudder + 10.0f * dt);
            } else if (!guiControlActive) {
                // Rudder returns to center slowly when no key pressed
                if (ownShipRudder > 0.5f) ownShipRudder -= 3.0f * dt;
                else if (ownShipRudder < -0.5f) ownShipRudder += 3.0f * dt;
                else ownShipRudder = 0;
            }

            // ===== SIMULATION MODEL UPDATE =====
            // Send controls to SimulationModel
            SimBridge::setPortEngine(ownShipPortEngine);
            SimBridge::setStbdEngine(ownShipStbdEngine);
            SimBridge::setWheel(ownShipRudder);
            SimBridge::setBowThruster(ownShipBowThruster);

            // Advance physics, AI, buoys, tide, wind, etc.
            if (frameCount <= 3) weLog("  Frame " + std::to_string(frameCount) + " pre-SimBridge::update()");
            try {
                SimBridge::update();
            } catch (const std::exception& e) {
                weLogErr("SimBridge::update() exception: " + std::string(e.what()));
            } catch (...) {
                weLogErr("SimBridge::update() unknown exception");
            }
            if (frameCount <= 3) weLog("  Frame " + std::to_string(frameCount) + " post-SimBridge::update()");

#ifdef _DEBUG
            // Periodic diagnostic log (every ~3 seconds) -- debug builds only
            static float diagTimer = 0;
            diagTimer += dt;
            if (diagTimer > 3.0f) {
                diagTimer = 0;
                float weTerrainH = terrainNode ? terrainNode->getHeightAt(SimBridge::getPosX(), SimBridge::getPosZ()) : -999;
                float weTerrainPosY = terrainNode ? terrainNode->getPosition().y : -999;
                weLog("  [DIAG] dt=" + std::to_string(dt) +
                      " eng=" + std::to_string(ownShipPortEngine) +
                      "/" + std::to_string(ownShipStbdEngine) +
                      " wheel=" + std::to_string(ownShipRudder) +
                      " SOG=" + std::to_string(SimBridge::getSOG()) +
                      " hdg=" + std::to_string(SimBridge::getHeading()) +
                      " x=" + std::to_string(SimBridge::getPosX()) +
                      " z=" + std::to_string(SimBridge::getPosZ()) +
                      " depth=" + std::to_string(SimBridge::getDepth()) +
                      " posY=" + std::to_string(SimBridge::getPosY()) +
                      " weH=" + std::to_string(weTerrainH) +
                      " wePosY=" + std::to_string(weTerrainPosY));
            }
#endif

            // Read own ship state back from SimulationModel
            ownShipX = SimBridge::getPosX();
            ownShipZ = SimBridge::getPosZ();
            ownShipHeading = SimBridge::getHeading();
            ownShipSpeed = SimBridge::getSOG() / KNOTS_TO_MPS; // m/s -> knots
            // Do NOT read rudder back into ownShipRudder -- that creates a feedback
            // loop (rudder lags behind wheel, making steering unresponsive).
            // ownShipRudder is the WHEEL command, SimBridge::getRudder() is the
            // actual rudder angle (used for HUD display only).
            beaufortScale = SimBridge::getWeather();

            // ===== DYNAMIC WEATHER & DAY/NIGHT CYCLE =====
            if (frameCount > 3) {
                float scenarioTime = SimBridge::getTimeDelta();
                float hourTime = std::fmod(scenarioTime, SECONDS_IN_DAY) / SECONDS_IN_HOUR;
                uint32_t lightLevel = SimBridge::getLightLevel();
                float ll = (float)lightLevel / 255.0f; // 0=dark, 1=bright

                // --- Sun position ---
                // WE derives light.direction by transforming (0,1,0) through the
                // world matrix, then copies it to weather.sunDirection for the sky.
                // We build a quaternion mapping (0,1,0) to the desired sun vector.
                float dayLen = sunSet - sunRise;
                float sunProgress = (dayLen > 0) ? (hourTime - sunRise) / dayLen : 0.5f;
                sunProgress = std::max(0.0f, std::min(1.0f, sunProgress));

                // Sun traces east -> zenith -> west semicircle
                float sdx = -std::cos((float)M_PI * sunProgress); // east at rise, west at set
                float sdy =  std::sin((float)M_PI * sunProgress); // up at noon
                float sdz = -0.3f;                                 // slight south arc
                float slen = std::sqrt(sdx*sdx + sdy*sdy + sdz*sdz);
                sdx /= slen; sdy /= slen; sdz /= slen;

                using namespace DirectX;
                XMVECTOR src = XMVectorSet(0, 1, 0, 0);
                XMVECTOR dst = XMVectorSet(sdx, sdy, sdz, 0);
                XMVECTOR axis = XMVector3Cross(src, dst);
                float axLen = XMVectorGetX(XMVector3Length(axis));
                XMVECTOR quat;
                if (axLen < 0.0001f) {
                    quat = XMQuaternionIdentity();
                } else {
                    axis = XMVector3Normalize(axis);
                    float dot = sdy;
                    float angle = std::acos(std::max(-1.0f, std::min(1.0f, dot)));
                    quat = XMQuaternionRotationAxis(axis, angle);
                }

                auto* st = scene.transforms.GetComponent(sunEntity);
                if (st) {
                    st->ClearTransform();
                    XMStoreFloat4(&st->rotation_local, quat);
                    st->SetDirty();
                    st->UpdateTransform();
                }

                // Sun intensity: fade over 0.5h twilight bands
                auto* sl = scene.lights.GetComponent(sunEntity);
                if (sl) {
                    float twilight = 1.0f;
                    if (hourTime < sunRise)
                        twilight = std::max(0.0f, 1.0f - (sunRise - hourTime) / 0.5f);
                    else if (hourTime > sunSet)
                        twilight = std::max(0.0f, 1.0f - (hourTime - sunSet) / 0.5f);
                    sl->intensity = 8.0f * twilight;

                    // Warm color near horizon (sunrise/sunset), white at zenith (noon)
                    // hFactor: 1.0 at sunrise/sunset, 0.0 at noon
                    float hFactor = std::min(1.0f, std::abs(sunProgress - 0.5f) * 4.0f);
                    sl->color = XMFLOAT3(1.0f, 1.0f - 0.3f * hFactor, 1.0f - 0.5f * hFactor);
                }

                // --- Ambient & stars ---
                scene.weather.ambient = XMFLOAT3(
                    0.05f + 0.25f * ll, 0.05f + 0.30f * ll, 0.08f + 0.32f * ll);
                scene.weather.stars = std::max(0.0f, 1.0f - ll * 2.0f);

                // --- Dynamic atmosphere: Mie scattering scales with Beaufort ---
                // More sea spray and aerosols in rough weather = more haze
                {
                    auto& atmo = scene.weather.atmosphereParameters;
                    float mieFactor = 1.0f + beaufortScale / 12.0f;
                    float mieBase = 0.006f * mieFactor;
                    atmo.mieScattering = XMFLOAT3(mieBase, mieBase, mieBase);
                    float mieExt = mieBase * 1.11f;
                    atmo.mieExtinction = XMFLOAT3(mieExt, mieExt, mieExt);
                    float mieAbs = mieExt - mieBase;
                    atmo.mieAbsorption = XMFLOAT3(mieAbs, mieAbs, mieAbs);
                    atmo.aerialPerspectiveScale = 1.5f + beaufortScale * 0.1f;
                }

                // --- Rain: disabled (WE bug: rain emitter particle buffer creation
                // crashes with SEH 0xC0000005 on descriptor index -1). ---
                // Rain value still drives cloud appearance below.
                float rain = SimBridge::getRain(); // 0-10

                // --- Visibility (needed by both clouds and fog) ---
                float vis = SimBridge::getVisibility(); // nautical miles
                if (vis <= 0) vis = 10.0f;

                // --- Dynamic clouds from Beaufort + visibility ---
                {
                    auto& vc = scene.weather.volumetricCloudParameters;
                    auto& L1 = vc.layerFirst;

                    // Two drivers: wind (Beaufort) and visibility (overcast)
                    float visCoverage = std::max(0.0f, 1.0f - vis / 8.0f);
                    float beaufortCoverage = std::min(1.0f, beaufortScale / 8.0f);
                    float overcastFactor = std::max(visCoverage, beaufortCoverage);
                    bool isOvercast = visCoverage > beaufortCoverage;

                    // Cloud base and thickness
                    if (isOvercast) {
                        vc.cloudStartHeight = 600.0f - visCoverage * 300.0f;
                        vc.cloudThickness = 1500.0f + visCoverage * 1000.0f;
                    } else {
                        vc.cloudStartHeight = 1500.0f - beaufortScale * 75.0f;
                        vc.cloudThickness = 4000.0f + beaufortScale * 500.0f;
                    }

                    // Coverage: overcast needs values well above WE default (1.0)
                    if (isOvercast) {
                        L1.coverageAmount = 1.5f + visCoverage * 0.5f;
                        L1.coverageMinimum = 0.5f + visCoverage * 0.3f;
                        L1.totalNoiseScale = 0.0003f;
                        L1.weatherScale = 0.000005f;
                        L1.detailNoiseModifier = 0.15f;
                    } else {
                        L1.coverageAmount = 0.5f + beaufortCoverage * 1.0f;
                        L1.coverageMinimum = 0.0f;
                        L1.totalNoiseScale = 0.0005f;
                        L1.weatherScale = 0.00002f;
                        L1.detailNoiseModifier = 0.3f;
                    }
                    L1.rainAmount = std::min(rain / 10.0f, 1.0f);

                    // Cloud type: flat stratus unless B7+ storms
                    if (beaufortScale > 7.0f) {
                        L1.typeAmount = std::min(1.0f, (beaufortScale - 7.0f) / 5.0f);
                    } else {
                        L1.typeAmount = 0.0f;
                    }

                    // Darken only in storms, overcast stays light grey
                    float stormDarken = std::max(0.0f, (beaufortScale - 6.0f) / 6.0f);
                    float cloudAlbedo = 0.9f - 0.15f * stormDarken;
                    L1.albedo = XMFLOAT3(cloudAlbedo, cloudAlbedo, cloudAlbedo);
                    float ext = isOvercast ? 0.05f : (0.071f + 0.03f * stormDarken);
                    L1.extinctionCoefficient = XMFLOAT3(0.71f * ext, 0.86f * ext, 1.0f * ext);

                    vc.ambientGroundMultiplier = isOvercast ? 0.75f : 0.6f;
                }

                // --- Wind drives clouds & atmosphere ---
                float windDir = SimBridge::getWindDirection(); // degrees FROM
                float windSpd = SimBridge::getWindSpeed();     // knots
                float windMps = windSpd * 0.514444f;
                float windRad = windDir * (float)M_PI / 180.0f;

                scene.weather.windDirection = XMFLOAT3(
                    std::sin(windRad), 0.0f, std::cos(windRad));
                scene.weather.windSpeed = windMps;

                scene.weather.volumetricCloudParameters.layerFirst.windAngle = windRad;
                scene.weather.volumetricCloudParameters.layerFirst.windSpeed = 10.0f + windMps * 2.0f;
                scene.weather.volumetricCloudParameters.layerFirst.coverageWindAngle = windRad;
                scene.weather.volumetricCloudParameters.layerFirst.coverageWindSpeed = 20.0f + windMps * 3.0f;

                // --- Dynamic fog with height fog ---
                float fogDist = vis * 1852.0f;
                scene.weather.fogStart = fogDist * 0.3f;
                scene.weather.fogDensity = (vis < 5.0f) ? (0.01f / std::max(vis, 0.1f)) : 0.0f;
                scene.weather.fogHeightStart = 0.0f;
                scene.weather.fogHeightEnd = 30.0f + (10.0f - std::min(vis, 10.0f)) * 20.0f;

                // --- Dynamic ocean (wave height, chop, wind direction, foam) ---
                if (frameCount > 5) {
                    float tideH = SimBridge::getTideHeight();
                    ocean.update(tideH,
                        bc::graphics::Vec3(camPosX, camPosY, camPosZ),
                        (int)lightLevel, beaufortScale, windSpd, windDir);
                }
            }

            // Guard against NaN/inf from SimulationModel (propagates to GPU and crashes DX12)
            if (std::isnan(ownShipX) || std::isinf(ownShipX)) { weLogErr("NaN/inf ownShipX"); ownShipX = 0; }
            if (std::isnan(ownShipZ) || std::isinf(ownShipZ)) { weLogErr("NaN/inf ownShipZ"); ownShipZ = 0; }
            if (std::isnan(ownShipHeading) || std::isinf(ownShipHeading)) { weLogErr("NaN/inf heading"); ownShipHeading = 0; }

            float headRad = ownShipHeading * (float)M_PI / 180.0f;

            // Ship sits at tide height + height correction. No wave heave, pitch,
            // or roll applied -- the bridge view must be rock-solid with zero sway.
            float tideY = SimBridge::getTideHeight();
            float ownShipY = ownShipHeightCorr + tideY;

            if (ownShipEntity != wi::ecs::INVALID_ENTITY) {
                setEntityTransform(scene, ownShipEntity, ownShipX, ownShipY, ownShipZ,
                                   ownShipHeading, ownShipScaleFactor,
                                   0.0f, 0.0f); // zero pitch/roll
            }

            // Shader-based Kelvin wake: write ship data into ocean constant buffer
            int wakeIdx = 0;
            {
                float spdMps = ownShipSpeed * KNOTS_TO_MPS;
                if (spdMps > 0.5f && wakeIdx < 8) {
                    auto& w = scene.weather.oceanParameters.wakeShips[wakeIdx++];
                    w.posX = ownShipX;
                    w.posZ = ownShipZ;
                    w.headingDirX = std::sin(headRad);
                    w.headingDirZ = std::cos(headRad);
                    w.speed = spdMps;
                    w.wakeLength = 150.0f;
                }
            }

            // Bridge camera: locked to ship with no wave-induced motion
            if (!camOrbitMode) {
                float vxScaled = viewLocalX * ownShipScaleFactor;
                float vzScaled = viewLocalZ * ownShipScaleFactor;
                camPosX = ownShipX + vxScaled * std::cos(headRad) + vzScaled * std::sin(headRad);
                camPosY = ownShipY + viewLocalY * ownShipScaleFactor;
                camPosZ = ownShipZ - vxScaled * std::sin(headRad) + vzScaled * std::cos(headRad);
                camPosY = std::max(camPosY, 2.0f);
            }

            // ===== OTHER SHIP POSITIONS (from SimulationModel AI) =====
            {
                int numOther = SimBridge::getNumberOfOtherShips();
                for (int s = 0; s < numOther && s < (int)otherShipStates.size(); s++) {
                    auto& st = otherShipStates[s];
                    st.x = SimBridge::getOtherShipPosX(s);
                    st.z = SimBridge::getOtherShipPosZ(s);
                    st.heading = SimBridge::getOtherShipHeading(s);
                    setEntityTransform(scene, st.entity, st.x, st.heightCorr, st.z,
                                       st.heading, st.scaleFactor);
                    float otherSpeedMps = SimBridge::getOtherShipSpeed(s);
                    if (otherSpeedMps > 0.5f && wakeIdx < 8) {
                        float hRad = st.heading * (float)M_PI / 180.0f;
                        auto& w = scene.weather.oceanParameters.wakeShips[wakeIdx++];
                        w.posX = st.x;
                        w.posZ = st.z;
                        w.headingDirX = std::sin(hRad);
                        w.headingDirZ = std::cos(hRad);
                        w.speed = otherSpeedMps;
                        w.wakeLength = 150.0f;
                    }
                }
            }
            scene.weather.oceanParameters.wakeShipCount = wakeIdx;

            // ===== BUOY POSITIONS (tidal movement from SimulationModel) =====
            {
                int numBuoys = SimBridge::getNumberOfBuoys();
                for (int b = 0; b < numBuoys && b < (int)buoyStates.size(); b++) {
                    auto& bs = buoyStates[b];
                    float bx = SimBridge::getBuoyPosX(b);
                    float bz = SimBridge::getBuoyPosZ(b);
                    setEntityTransform(scene, bs.entity, bx, bs.heightCorr, bz,
                                       0, bs.scaleFactor);
                }
            }

            // ===== NAVIGATION LIGHTS (own ship, other ships, buoys, land) =====
            {
                float scenarioTime = SimBridge::getTimeDelta();
                uint32_t lightLevel = SimBridge::getLightLevel();
                float lightAlpha = (255.0f - (float)lightLevel) / 255.0f; // 0=invisible(day), 1=bright(night)

                for (auto& nlt : navLights) {
                    if (nlt.entity == wi::ecs::INVALID_ENTITY) continue;

                    float wx, wy, wz;
                    float bearingRef = 0; // heading for sector checks (0 = true north for buoy/land)

                    if (nlt.buoyIndex >= 0 && nlt.buoyIndex < (int)buoyStates.size()) {
                        // Buoy-attached light: get buoy position + height offset
                        wx = SimBridge::getBuoyPosX(nlt.buoyIndex);
                        wz = SimBridge::getBuoyPosZ(nlt.buoyIndex);
                        wy = buoyStates[nlt.buoyIndex].heightCorr + nlt.localY;
                        bearingRef = 0; // sectors relative to true north
                    } else if (nlt.shipIndex == -2) {
                        // Fixed land light: world coords stored in localX/Y/Z
                        wx = nlt.localX;
                        wy = nlt.localY;
                        wz = nlt.localZ;
                        bearingRef = 0; // sectors relative to true north
                    } else if (nlt.shipIndex == -1) {
                        // Own ship
                        float headRad = ownShipHeading * (float)M_PI / 180.0f;
                        float cosH = std::cos(headRad), sinH = std::sin(headRad);
                        wx = ownShipX + (nlt.localX * cosH + nlt.localZ * sinH) * ownShipScaleFactor;
                        wy = ownShipHeightCorr + nlt.localY * ownShipScaleFactor;
                        wz = ownShipZ + (-nlt.localX * sinH + nlt.localZ * cosH) * ownShipScaleFactor;
                        bearingRef = ownShipHeading;
                    } else {
                        // Other ship
                        if (nlt.shipIndex >= (int)otherShipStates.size()) continue;
                        const auto& ship = otherShipStates[nlt.shipIndex];
                        float headRad = ship.heading * (float)M_PI / 180.0f;
                        float cosH = std::cos(headRad), sinH = std::sin(headRad);
                        wx = ship.x + (nlt.localX * cosH + nlt.localZ * sinH) * ship.scaleFactor;
                        wy = ship.heightCorr + nlt.localY * ship.scaleFactor;
                        wz = ship.z + (-nlt.localX * sinH + nlt.localZ * cosH) * ship.scaleFactor;
                        bearingRef = ship.heading;
                    }

                    // Distance and basic night check
                    float dx = wx - camPosX, dz = wz - camPosZ, dy = wy - camPosY;
                    float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
                    bool inRange = (dist <= nlt.range) && (lightAlpha > 0.05f);

                    // Arc visibility: is the observer within the light's directional sector?
                    bool arcVisible = true;
                    if (inRange && !(nlt.startAngle == 0 && nlt.endAngle == 360)) {
                        // Bearing FROM light TO observer (compass: 0=N, 90=E)
                        float angleToCamera = std::atan2(-dx, -dz) * 180.0f / (float)M_PI;
                        float localAngle = angleToCamera - bearingRef;
                        while (localAngle < 0) localAngle += 360;
                        while (localAngle >= 360) localAngle -= 360;
                        float sa = nlt.startAngle, ea = nlt.endAngle;
                        while (sa < 0) sa += 360;
                        while (sa >= 360) sa -= 360;
                        while (ea < 0) ea += 360;
                        while (ea >= 360) ea -= 360;
                        if (sa == ea) {
                            arcVisible = false;
                        } else if (sa < ea) {
                            arcVisible = (localAngle >= sa && localAngle <= ea);
                        } else {
                            arcVisible = (localAngle >= sa || localAngle <= ea);
                        }
                    }

                    // Flash sequence check
                    bool flashOn = true;
                    if (!nlt.sequence.empty()) {
                        size_t seqLen = nlt.sequence.length();
                        float timeInSeq = std::fmod((scenarioTime + nlt.timeOffset) / nlt.charTime, (float)seqLen);
                        size_t pos = (size_t)timeInSeq;
                        if (pos >= seqLen) pos = seqLen - 1;
                        if (nlt.sequence[pos] == 'D' || nlt.sequence[pos] == 'd')
                            flashOn = false;
                    }

                    // Unified visibility: intensity=0 hides both PBR illumination AND lens flare.
                    // Own ship: skip arc check (camera is ON the bridge, physically outside
                    // most arcs). WE depth buffer handles occlusion for own-hull geometry.
                    // Other ships/buoys/land: arc check determines sector visibility.
                    bool visible;
                    if (nlt.shipIndex == -1) {
                        visible = inRange && flashOn;
                    } else {
                        visible = inRange && arcVisible && flashOn;
                    }

                    // Position the light (lens flare position is derived from the light entity)
                    setEntityTransform(scene, nlt.entity, wx, wy, wz);

                    auto* lightComp = scene.lights.GetComponent(nlt.entity);
                    if (lightComp) {
                        if (!visible) {
                            lightComp->intensity = 0.0f; // hides lens flare too (IsInactive)
                        } else {
                            // Allard's Law: illuminance = I * T^D / D^2
                            // T=0.8 per NM (10nm meteorological visibility, clear night)
                            float D_nm = std::max(0.1f, dist / 1852.0f);
                            float transmittance = std::pow(0.8f, D_nm);
                            float illuminance = nlt.intensity_cd * transmittance / (D_nm * D_nm);
                            // Scale to WE intensity units. With PBR range=30m and
                            // volumetrics off, the light only illuminates nearby hull/buoy.
                            // The 4px lens flare dot provides distant visibility.
                            // Intensity just needs to be non-zero (keeps flare active)
                            // and proportional to distance for subtle close-range bloom.
                            float weIntensity = std::min(3000.0f, illuminance * 50.0f) * lightAlpha;
                            lightComp->intensity = weIntensity;
                        }
                    }
                }
            }

            // One-shot diagnostic: log first 5 nav lights' state
            {
                static bool diagDone = false;
                if (!diagDone && navLights.size() > 0) {
                    diagDone = true;
                    int count = std::min((int)navLights.size(), 15);
                    for (int i = 0; i < count; i++) {
                        auto& nlt = navLights[i];
                        auto* lc = scene.lights.GetComponent(nlt.entity);
                        std::string msg = "  [DIAG] light " + std::to_string(i) +
                            " entity=" + std::to_string(nlt.entity) +
                            " intensity=" + (lc ? std::to_string(lc->intensity) : "NO_LC") +
                            " range=" + (lc ? std::to_string(lc->range) : "?") +
                            " flares=" + (lc ? std::to_string(lc->lensFlareRimTextures.size()) : "?") +
                            " inactive=" + (lc ? std::to_string(lc->IsInactive()) : "?") +
                            " intensity_cd=" + std::to_string(nlt.intensity_cd) +
                            " shipIdx=" + std::to_string(nlt.shipIndex);
                        weLog(msg);
                    }
                    weLog("  [DIAG] lensFlareEnabled=" + std::to_string(renderPath.getLensFlareEnabled()));
                    weLog("  [DIAG] total navLights=" + std::to_string(navLights.size()));
                }
            }

            // ===== RADAR DATA UPDATE =====
            // Write radar pixels to UPLOAD staging texture; the actual GPU copy
            // happens in BCRenderPath::Compose() on the GRAPHICS queue command list.
            {
                static int radarUpdateCounter = 0;
                if (++radarUpdateCounter >= 4) {
                    radarUpdateCounter = 0;
                    const int RS = RADAR_TEX_SIZE;
                    bool ok = SimBridge::getRadarImage(radarPixels, RS);
                    static int radarFailCount = 0;
                    if (!ok) {
                        if (++radarFailCount <= 3) {
                            weLog("  Radar getImage FAILED (attempt " +
                                  std::to_string(radarFailCount) + ", imgSize=" +
                                  std::to_string(SimBridge::getRadarImageSize()) + ")");
                        }
                    }
                    static bool radarDataLogged = false;
                    if (!radarDataLogged && ok) {
                        int mid = RS / 2;
                        int idx = (mid * RS + mid) * 4;
                        weLog("  Radar data: src=" + std::to_string(SimBridge::getRadarImageSize()) +
                              " center RGBA=(" +
                              std::to_string(radarPixels[idx]) + "," +
                              std::to_string(radarPixels[idx+1]) + "," +
                              std::to_string(radarPixels[idx+2]) + "," +
                              std::to_string(radarPixels[idx+3]) + ")");
                        radarDataLogged = true;
                    }
                    if (ok) {
                        // Recreate the texture with new pixel data.
                        // CreateTexture handles staging internally (CopyAllocator).
                        wi::graphics::TextureDesc texDesc;
                        texDesc.width = RS;
                        texDesc.height = RS;
                        texDesc.format = wi::graphics::Format::R8G8B8A8_UNORM;
                        texDesc.bind_flags = wi::graphics::BindFlag::SHADER_RESOURCE;
                        texDesc.mip_levels = 1;
                        texDesc.array_size = 1;
                        wi::graphics::SubresourceData texData;
                        texData.data_ptr = radarPixels;
                        texData.row_pitch = RS * 4;
                        texData.slice_pitch = texData.row_pitch * RS;
                        wi::graphics::GetDevice()->CreateTexture(&texDesc, &texData, &g_radarTex);

                        // Update 3D screen material: SetTexture copies the new shared_ptr,
                        // CreateRenderData rebuilds GPU descriptors for the new resource.
                        if (radarScreenEntity != wi::ecs::INVALID_ENTITY) {
                            auto* radarMat = scene.materials.GetComponent(radarMaterialEntity);
                            if (radarMat) {
                                radarMat->textures[wi::scene::MaterialComponent::EMISSIVEMAP].resource.SetTexture(g_radarTex);
                                radarMat->CreateRenderData();
                            }
                        }
                    }
                }
            }

            // ===== RADAR 3D SCREEN POSITION =====
            if (radarScreenEntity != wi::ecs::INVALID_ENTITY) {
                float screenOffX = radarLocalX - 0.02f;
                float screenOffY = radarLocalY;
                float screenOffZ = radarLocalZ + 0.08f;
                float cosH = std::cos(headRad), sinH = std::sin(headRad);
                float rwx = ownShipX + (screenOffX * cosH + screenOffZ * sinH);
                float rwy = screenOffY + (ownShipY - ownShipHeightCorr) * 0.3f;
                float rwz = ownShipZ + (-screenOffX * sinH + screenOffZ * cosH);
                auto* xform = scene.transforms.GetComponent(radarScreenEntity);
                if (xform) {
                    xform->ClearTransform();
                    xform->Translate(DirectX::XMFLOAT3(rwx, rwy, rwz));
                    xform->RotateRollPitchYaw(DirectX::XMFLOAT3(0, headRad, 0));
                    xform->UpdateTransform();
                }
            }

            // ===== MAP SCREEN PER-FRAME UPDATE =====
            if (mapScreenEntity != wi::ecs::INVALID_ENTITY && mapScreen) {
                // Position map screen to follow ship
                float mapOffX = mapLocalX;
                float mapOffY = mapLocalY;
                float mapOffZ = mapLocalZ;
                float mCosH = std::cos(headRad), mSinH = std::sin(headRad);
                float mwx = ownShipX + (mapOffX * mCosH + mapOffZ * mSinH);
                float mwy = mapOffY + (ownShipY - ownShipHeightCorr) * 0.3f;
                float mwz = ownShipZ + (-mapOffX * mSinH + mapOffZ * mCosH);
                auto* mxform = scene.transforms.GetComponent(mapScreenEntity);
                if (mxform) {
                    mxform->ClearTransform();
                    mxform->Translate(DirectX::XMFLOAT3(mwx, mwy, mwz));
                    float mapYaw = headRad + mapScreenAngle * (float)M_PI / 180.0f;
                    mxform->RotateRollPitchYaw(DirectX::XMFLOAT3(0, mapYaw, 0));
                    mxform->UpdateTransform();
                }

                // Update map every 8th frame (~15Hz at 120fps)
                static int mapUpdateCounter = 0;
                if (++mapUpdateCounter >= 8) {
                    mapUpdateCounter = 0;

                    double ownLat = coords.zToLat(ownShipZ);
                    double ownLon = coords.xToLong(ownShipX);

                    // Gather AIS contacts from other ships
                    int nOther = SimBridge::getNumberOfOtherShips();
                    std::vector<MapScreen::AISContact> contacts(nOther);
                    for (int ci = 0; ci < nOther; ci++) {
                        contacts[ci].lat = coords.zToLat(SimBridge::getOtherShipPosZ(ci));
                        contacts[ci].lon = coords.xToLong(SimBridge::getOtherShipPosX(ci));
                        contacts[ci].heading = SimBridge::getOtherShipHeading(ci);
                        contacts[ci].speed = SimBridge::getOtherShipSpeed(ci) * 1.94384f; // m/s to knots
                    }

                    mapScreen->update(ownLat, ownLon, ownShipHeading,
                                      contacts.data(), nOther, mapZoom);

                    // Upload pixels to GPU
                    static wi::graphics::Texture mapGPUTexUpdate;
                    wi::graphics::TextureDesc mtd;
                    mtd.width = MAP_TEX_SIZE;
                    mtd.height = MAP_TEX_SIZE;
                    mtd.format = wi::graphics::Format::R8G8B8A8_UNORM;
                    mtd.bind_flags = wi::graphics::BindFlag::SHADER_RESOURCE;
                    mtd.mip_levels = 1;
                    mtd.array_size = 1;
                    wi::graphics::SubresourceData msd;
                    msd.data_ptr = mapScreen->getPixels();
                    msd.row_pitch = MAP_TEX_SIZE * 4;
                    msd.slice_pitch = msd.row_pitch * MAP_TEX_SIZE;
                    wi::graphics::GetDevice()->CreateTexture(&mtd, &msd, &mapGPUTexUpdate);

                    auto* mapMat = scene.materials.GetComponent(mapMaterialEntity);
                    if (mapMat) {
                        mapMat->textures[wi::scene::MaterialComponent::EMISSIVEMAP].resource.SetTexture(mapGPUTexUpdate);
                        mapMat->SetDirty();
                    }
                }
            }

            // ===== ECDIS DATA FEED =====
            if (showEcdisFullscreen) {
                bc::gui::EcdisDisplay::OwnShipData ecdisShip;
                ecdisShip.lat = coords.zToLat(ownShipZ);
                ecdisShip.lon = coords.xToLong(ownShipX);
                ecdisShip.heading = SimBridge::getHeading();
                ecdisShip.cog = SimBridge::getCOG();
                ecdisShip.sog = SimBridge::getSOG() / KNOTS_TO_MPS;
                ecdisShip.stw = SimBridge::getSTW() / KNOTS_TO_MPS;
                ecdisShip.depth = SimBridge::getDepth();
                ecdisShip.windSpeed = SimBridge::getWindSpeed();
                ecdisShip.windDirection = SimBridge::getWindDirection();
                ecdisShip.rudder = SimBridge::getRudder();
                ecdisShip.simulationTime = totalSimTime;
                ecdisShip.tidalStreamX = 0; // Phase 4
                ecdisShip.tidalStreamZ = 0;
                ecdisDisplay.setOwnShipData(ecdisShip);

                int nOther = SimBridge::getNumberOfOtherShips();
                std::vector<bc::gui::EcdisDisplay::AISTarget> ecdisAIS(nOther);
                for (int ci = 0; ci < nOther; ci++) {
                    ecdisAIS[ci].lat = coords.zToLat(SimBridge::getOtherShipPosZ(ci));
                    ecdisAIS[ci].lon = coords.xToLong(SimBridge::getOtherShipPosX(ci));
                    ecdisAIS[ci].heading = SimBridge::getOtherShipHeading(ci);
                    ecdisAIS[ci].cog = SimBridge::getOtherShipHeading(ci); // COG approx = heading
                    ecdisAIS[ci].speed = SimBridge::getOtherShipSpeed(ci) * 1.94384f;
                    ecdisAIS[ci].id = ci + 1;
                    ecdisAIS[ci].mmsi = SimBridge::getOtherShipMMSI(ci);
                    ecdisAIS[ci].name = SimBridge::getOtherShipName(ci);
                    ecdisAIS[ci].length = SimBridge::getOtherShipLength(ci);
                    ecdisAIS[ci].breadth = SimBridge::getOtherShipBreadth(ci);
                }
                ecdisDisplay.setAISTargets(ecdisAIS);
            }

            // Toggle orbit/bridge mode with 'O'
            bool keyODown = (GetAsyncKeyState('O') & 0x8000) != 0;
            if (keyODown && !keyOWasDown) {
                camOrbitMode = !camOrbitMode;
                if (camOrbitMode) {
                    camTargetX = ownShipX;
                    camTargetY = camPosY;
                    camTargetZ = ownShipZ;
                    camDistance = 200.0f;
                    camPitch = 25.0f;
                } else {
                    // Reset bridge view to forward-looking default
                    camPitch = 0.0f;
                    camYawOffset = 0.0f;
                    walkLocalX = 0.0f;
                    walkLocalZ = 0.0f;
                }
                // Ship stays visible in both modes: orbit sees it from outside,
                // bridge mode sees the bridge interior from inside.
            }
            keyOWasDown = keyODown;

            float moveSpeed = 2.0f;
            if (GetAsyncKeyState(VK_SHIFT) & 0x8000) moveSpeed = 10.0f;

            // Mouse look / orbit (left or right drag)
            POINT mousePos;
            GetCursorPos(&mousePos);
            ScreenToClient(hWnd, &mousePos);

            bool lbDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            bool rbDown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
            bool imguiWantsMouse = bc::graphics::wicked::ImGuiWantsMouse();
            bool anyMouseDrag = (lbDown || rbDown) && !imguiWantsMouse;

            if (anyMouseDrag) {
                if (!mouseLeftDown && !mouseRightDown) {
                    lastMouseX = mousePos.x;
                    lastMouseY = mousePos.y;
                }
                int dx = mousePos.x - lastMouseX;
                int dy = mousePos.y - lastMouseY;
                if (camOrbitMode) {
                    camYaw += dx * 0.3f;
                } else {
                    camYawOffset += dx * 0.3f;
                }
                camPitch += dy * 0.3f;
                camPitch = std::max(-89.0f, std::min(89.0f, camPitch));
                lastMouseX = mousePos.x;
                lastMouseY = mousePos.y;
            }
            mouseLeftDown = lbDown;
            mouseRightDown = rbDown;

            // Yaw in WE left-handed space (heading convention)
            // In bridge mode, camYaw = ownShipHeading + camYawOffset (set above in ship controls)
            float effectiveYaw = camOrbitMode ? camYaw : (ownShipHeading + camYawOffset);
            float yawRad = effectiveYaw * (float)M_PI / 180.0f;

            // Scroll wheel: zoom in orbit, FOV in bridge
            float scroll = wi::input::GetPointer().z;
            if (scroll != 0) {
                if (camOrbitMode) {
                    camDistance -= scroll * camDistance * 0.1f;
                    camDistance = std::max(5.0f, std::min(50000.0f, camDistance));
                } else {
                    cameraFov -= scroll * 3.0f;
                    cameraFov = std::max(20.0f, std::min(120.0f, cameraFov));
                }
            }

            float camX, camY, camZ, lookX, lookY, lookZ;

            if (camOrbitMode) {
                // WASD moves the orbit target
                if (GetAsyncKeyState('W') & 0x8000) {
                    camTargetX += std::sin(yawRad) * moveSpeed;
                    camTargetZ += std::cos(yawRad) * moveSpeed;
                }
                if (GetAsyncKeyState('S') & 0x8000) {
                    camTargetX -= std::sin(yawRad) * moveSpeed;
                    camTargetZ -= std::cos(yawRad) * moveSpeed;
                }
                if (GetAsyncKeyState('A') & 0x8000) {
                    camTargetX -= std::cos(yawRad) * moveSpeed;
                    camTargetZ += std::sin(yawRad) * moveSpeed;
                }
                if (GetAsyncKeyState('D') & 0x8000) {
                    camTargetX += std::cos(yawRad) * moveSpeed;
                    camTargetZ -= std::sin(yawRad) * moveSpeed;
                }

                float pitchRad = camPitch * (float)M_PI / 180.0f;
                camX = camTargetX - camDistance * std::sin(yawRad) * std::cos(pitchRad);
                camY = camTargetY + camDistance * std::sin(pitchRad) + 5.0f;
                camZ = camTargetZ - camDistance * std::cos(yawRad) * std::cos(pitchRad);
                lookX = camTargetX;
                lookY = camTargetY;
                lookZ = camTargetZ;
            } else {
                // Bridge first-person mode with WASD walking
                // Walk direction is relative to camera look (in ship-local frame)
                float walkYawRad = camYawOffset * (float)M_PI / 180.0f;
                float walkSpeed = 1.5f * dt; // ~1.5 m/s walk
                if (GetAsyncKeyState(VK_SHIFT) & 0x8000) walkSpeed = 4.0f * dt;

                if (GetAsyncKeyState('W') & 0x8000) {
                    walkLocalX += std::sin(walkYawRad) * walkSpeed;
                    walkLocalZ += std::cos(walkYawRad) * walkSpeed;
                }
                if (GetAsyncKeyState('S') & 0x8000) {
                    walkLocalX -= std::sin(walkYawRad) * walkSpeed;
                    walkLocalZ -= std::cos(walkYawRad) * walkSpeed;
                }
                if (GetAsyncKeyState('A') & 0x8000) {
                    walkLocalX -= std::cos(walkYawRad) * walkSpeed;
                    walkLocalZ += std::sin(walkYawRad) * walkSpeed;
                }
                if (GetAsyncKeyState('D') & 0x8000) {
                    walkLocalX += std::cos(walkYawRad) * walkSpeed;
                    walkLocalZ -= std::sin(walkYawRad) * walkSpeed;
                }

                // R resets walk position to default bridge view
                if (GetAsyncKeyState('R') & 0x8000) {
                    walkLocalX = 0.0f;
                    walkLocalZ = 0.0f;
                }

                // Clamp to bridge bounds (rectangular, from view positions)
                walkLocalX = std::max(-bridgeHalfW, std::min(bridgeHalfW, walkLocalX));
                walkLocalZ = std::max(-bridgeHalfD, std::min(bridgeHalfD, walkLocalZ));

                // Transform ship-local walk offset to world space and apply
                float cosH = std::cos(headRad);
                float sinH = std::sin(headRad);
                camX = camPosX + walkLocalX * cosH + walkLocalZ * sinH;
                camY = camPosY;
                camZ = camPosZ - walkLocalX * sinH + walkLocalZ * cosH;

                // Look direction from yaw/pitch
                float pitchRad = camPitch * (float)M_PI / 180.0f;
                lookX = camX + std::sin(yawRad) * std::cos(pitchRad) * 100.0f;
                lookY = camY - std::sin(pitchRad) * 100.0f;
                lookZ = camZ + std::cos(yawRad) * std::cos(pitchRad) * 100.0f;
            }

            // Apply camera
            float fovRad = cameraFov * (float)M_PI / 180.0f;
            camera.zNearP = 0.5f;
            camera.zFarP = 50000.0f;
            camera.fov = fovRad;

            // Guard camera values against NaN/inf
            if (std::isnan(camX) || std::isinf(camX) || std::isnan(camY) || std::isinf(camY) ||
                std::isnan(camZ) || std::isinf(camZ)) {
                weLogErr("NaN/inf camera position, resetting to origin");
                camX = 0; camY = 50; camZ = 0;
                lookX = 0; lookY = 50; lookZ = 100;
            }

            if (frameCount <= 3) weLog("  Frame " + std::to_string(frameCount) + " pre-camera cam=(" +
                std::to_string(camX) + "," + std::to_string(camY) + "," + std::to_string(camZ) + ") look=(" +
                std::to_string(lookX) + "," + std::to_string(lookY) + "," + std::to_string(lookZ) + ")");
            DirectX::XMVECTOR vEye = DirectX::XMVectorSet(camX, camY, camZ, 1.0f);
            DirectX::XMVECTOR vAt = DirectX::XMVectorSet(lookX, lookY, lookZ, 1.0f);
            DirectX::XMVECTOR vUp = DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
            DirectX::XMMATRIX viewMat = DirectX::XMMatrixLookAtLH(vEye, vAt, vUp);
            DirectX::XMMATRIX invView = DirectX::XMMatrixInverse(nullptr, viewMat);
            camera.TransformCamera(invView);
            if (frameCount <= 3) weLog("  Frame " + std::to_string(frameCount) + " post-camera");

            // Skip rendering when window is inactive (minimized or lost focus).
            // Must check BEFORE ImGuiNewFrame() to avoid NewFrame/EndFrame mismatch.
            // If focus is lost DURING application.Run(), Run() may skip Compose()
            // (which calls ImGui::Render()), leaving an unbalanced NewFrame -> crash.
            if (!application.is_window_active) {
                Sleep(16);
                continue;
            }

            // ===== IMGUI HUD =====
            {
                // Win32 backend handles DisplaySize, DeltaTime, mouse, keyboard, scroll
                ImGui_ImplWin32_NewFrame();
                bc::graphics::wicked::ImGuiNewFrame();

                // Pass current control values to overlay sliders
                overlay.setControlValues(ownShipPortEngine, ownShipStbdEngine, ownShipRudder, ownShipBowThruster);

                // Populate HUD data from SimulationModel
                bc::gui::SimulationHUDData hudData;
                hudData.heading = SimBridge::getHeading();
                hudData.courseOverGround = SimBridge::getCOG();
                hudData.speedOverGround = SimBridge::getSOG() / KNOTS_TO_MPS; // knots
                hudData.speedThroughWater = SimBridge::getSTW() / KNOTS_TO_MPS; // knots
                hudData.rudderAngle = SimBridge::getRudder();
                hudData.thrustLever = SimBridge::getPortEngine();
                hudData.engineRPM = SimBridge::getPortEngineRPM();
                hudData.wheelAngle = SimBridge::getWheel();
                hudData.depth = SimBridge::getDepth();
                hudData.rateOfTurn = SimBridge::getRateOfTurn();
                hudData.windSpeed = SimBridge::getWindSpeed();
                hudData.windDirection = SimBridge::getWindDirection();
                hudData.simulationTime = totalSimTime;
                overlay.setSimulationData(hudData);
                if (!showRadarFullscreen && !showEcdisFullscreen) {
                    overlay.render();
                }

                // --- ESC Menu overlay ---
                if (showEscMenu) {
                    // Semi-transparent background
                    ImGui::SetNextWindowPos(ImVec2(width * 0.5f, height * 0.5f),
                        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
                    ImGui::SetNextWindowSize(ImVec2(260, 0), ImGuiCond_Always);
                    ImGuiWindowFlags menuFlags = ImGuiWindowFlags_NoCollapse |
                        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                        ImGuiWindowFlags_AlwaysAutoResize;
                    ImGui::Begin("Paused", nullptr, menuFlags);

                    float btnW = ImGui::GetContentRegionAvail().x;
                    if (ImGui::Button("Resume", ImVec2(btnW, 40))) {
                        showEscMenu = false;
                    }
                    if (ImGui::Button("Settings", ImVec2(btnW, 40))) {
                        showSettings = true;
                    }
                    if (ImGui::Button("Radar", ImVec2(btnW, 40))) {
                        showRadarFullscreen = true;
                        showEcdisFullscreen = false;
                        showEscMenu = false;
                    }
                    if (ImGui::Button("ECDIS", ImVec2(btnW, 40))) {
                        showEcdisFullscreen = true;
                        showRadarFullscreen = false;
                        showEscMenu = false;
                    }
                    ImGui::Separator();
                    if (ImGui::Button("Exit", ImVec2(btnW, 40))) {
                        weLog("Exit pressed from ESC menu");
                        PostQuitMessage(0);
                    }

                    ImGui::End();
                }

                // --- Settings panel ---
                if (showSettings) {
                    if (!settingsPanel.render(width, height)) {
                        showSettings = false;
                    }
                }

                // --- Full-screen radar display ---
                if (showRadarFullscreen) {
                    ImTextureID radarTexID = g_radarTex.IsValid() ? (ImTextureID)&g_radarTex : nullptr;
                    if (!radarDisplay.render(width, height, radarTexID, RADAR_TEX_SIZE)) {
                        showRadarFullscreen = false;
                    }
                }

                // --- Full-screen ECDIS display ---
                if (showEcdisFullscreen) {
                    if (!ecdisDisplay.render(width, height)) {
                        showEcdisFullscreen = false;
                    }
                }

                // Read back control values (may have been modified by GUI sliders)
                ownShipPortEngine = overlay.getControlPortEngine();
                ownShipStbdEngine = overlay.getControlStbdEngine();
                ownShipRudder = overlay.getControlWheel();
                ownShipBowThruster = overlay.getControlBowThruster();
            }

            // ===== SOUND UPDATE =====
            // Engine/alarm sounds handled by SimulationModel.
            // Horn triggered by keyboard here.
            {
                static bool hornActive = false;
                if (GetAsyncKeyState('H') & 0x8000) {
                    if (!hornActive) { sound.setVolumeHorn(1.0f); hornActive = true; }
                } else {
                    if (hornActive) { sound.setVolumeHorn(0.0f); hornActive = false; }
                }
            }

            // Update window title
            {
                int hdg = (int)std::round(ownShipHeading) % 360;
                char buf[256];
                snprintf(buf, sizeof(buf),
                    "Bridge Command (WE DX12) | HDG %03d | SPD %.1f kn | ENG %d%% | RUD %d",
                    hdg, std::abs(ownShipSpeed),
                    (int)std::round((ownShipPortEngine + ownShipStbdEngine) * 50.0f),
                    (int)std::round(ownShipRudder));
                SetWindowTextA(hWnd, buf);
            }

            if (frameCount <= 5)
                weLog("  Frame " + std::to_string(frameCount) + " pre-Run()");

            if (!runAppWithSEH(application)) {
                char crashBuf[256];
                snprintf(crashBuf, sizeof(crashBuf),
                    "CRASH in application.Run(): SEH exception 0x%08lX (frame=%d, cam Y=%.1f, ship Y=%.1f)",
                    g_lastSEHCode, frameCount, camPosY, ownShipY);
                weLogErr(crashBuf);
                PostQuitMessage(1);
            }

            // Safety: if Run() skipped Compose() (e.g. focus lost during render),
            // ImGui::Render() was never called. EndFrame() no-ops if already called.
            ImGui::EndFrame();

            if (frameCount <= 5)
                weLog("  Frame " + std::to_string(frameCount) + " post-Run() OK");

            // Heartbeat log every 100 frames to pinpoint crash timing
            if (frameCount % 100 == 0) {
                weLog("  [HEARTBEAT] frame=" + std::to_string(frameCount) +
                      " t=" + std::to_string(totalSimTime) + "s" +
                      " dt=" + std::to_string(dt) +
                      " ship=(" + std::to_string(ownShipX) + "," +
                      std::to_string(ownShipY) + "," + std::to_string(ownShipZ) + ")" +
                      " hdg=" + std::to_string(ownShipHeading) +
                      " spd=" + std::to_string(ownShipSpeed));
            }

            // Flush log every 500 frames to ensure we capture data before a crash
            if (frameCount % 500 == 0 && g_weLog.is_open()) {
                g_weLog.flush();
            }

            // Screenshot support: F12 key or file-watch trigger.
            // File-watch: external tool writes "screenshot_request.txt", we save
            // "screenshot.png" and delete the request file to signal completion.
            {
                static bool f12WasDown = false;
                bool f12IsDown = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
                bool fileRequest = false;
                // Check file every 30 frames to reduce I/O
                if (frameCount % 30 == 0) {
                    FILE* fp = fopen(screenshotRequestPath.c_str(), "r");
                    if (fp) { fclose(fp); fileRequest = true; }
                }

                if ((f12IsDown && !f12WasDown) || fileRequest) {
                    weLog("Screenshot triggered (file=" + std::to_string(fileRequest) +
                          " path=" + screenshotRequestPath + ")");
                    std::string outPath = fileRequest ? screenshotOutputPath : "";
                    std::string result = wi::helper::screenshot(application.swapChain, outPath);
                    if (!result.empty()) {
                        weLog("Screenshot saved: " + result);
                    } else {
                        weLog("Screenshot FAILED (empty result)");
                    }
                    if (fileRequest) {
                        std::remove(screenshotRequestPath.c_str());
                    }
                }
                f12WasDown = f12IsDown;
            }
        }
    }

    // Log why we exited the render loop
    weLog("Render loop ended: msg.message=" + std::to_string(msg.message) +
          " wParam=" + std::to_string(msg.wParam) +
          " frame=" + std::to_string(frameCount));

    // Cleanup
    weLog("Shutting down SimulationBridge...");
    SimBridge::shutdown();
    weLog("Shutting down Wicked Engine...");
    ImGui_ImplWin32_Shutdown();
    bc::graphics::wicked::ImGuiShutdown();
    g_convertedMeshCache.clear();
    g_sharedPlaceholderMeshes.clear();
    terrainNode.reset();
    g_weApp = nullptr;
    wi::jobsystem::ShutDown();

    DestroyWindow(hWnd);
    UnregisterClassW(wcex.lpszClassName, hInstance);

    weLog("Wicked Engine shutdown complete.");
    g_weLog.close();
    return EXIT_SUCCESS;
}

#endif // WITH_WICKED_ENGINE
