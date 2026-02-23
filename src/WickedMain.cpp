#ifdef WITH_WICKED_ENGINE

#include "WickedMain.hpp"

// WickedEngine must be included before any Irrlicht headers
#include "WickedEngine.h"

#include "graphics/wicked/WickedWater.hpp"
#include "graphics/wicked/WickedTerrainNode.hpp"
#include "graphics/wicked/WickedModelImporter.hpp"
#include "graphics/wicked/WickedImGui.hpp"
#include "gui/ImGuiOverlay.hpp"
#include "IrrlichtModelConverter.hpp"
#include "SimulationBridge.hpp"
#include "BuildingGenerator.hpp"
#include "editor/OSMBuildingReader.hpp"
#include "IniFile.hpp"
#include "Utilities.hpp"
#include "Constants.hpp"
#include "Sound.hpp"

// ImGui header needed for IO access in game loop
#include "graphics/wicked/imgui/imgui.h"

#include <iostream>
#include <fstream>
#include <cmath>
#include <cstdlib>
#include <vector>
#include <string>
#include <algorithm>
#include <chrono>
#include <unordered_map>

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
static bool mouseRightDown = false;
static bool mouseLeftDown = false;
static int lastMouseX = 0, lastMouseY = 0;

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
};

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

// Cache of WE mesh entities created from Irrlicht-converted models (by filepath)
static std::unordered_map<std::string, wi::ecs::Entity> g_convertedMeshCache;

// Create a WE mesh entity from Irrlicht-converted model data (standalone, not attached to any root)
// textureNames: optional list of texture filenames scanned from the model file
// modelDir: directory containing the model (for resolving relative texture paths)
static wi::ecs::Entity createWEMeshFromConverted(wi::scene::Scene& scene,
                                                   const bc::ConvertedModel& model,
                                                   const std::string& baseName,
                                                   const std::vector<std::string>& textureNames = {},
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

            // Color-only models (no texture) look flat at roughness=1.0;
            // give them a moderate sheen like painted concrete/plaster
            if (sub.material.textureName.empty() && roughness > 0.7f) {
                material->roughness = 0.55f;
            }

            // Assign texture: prefer per-submesh name from Irrlicht (correct mapping),
            // fall back to scanned names by index (approximate)
            std::string texPath;
            if (!sub.material.textureName.empty()) {
                texPath = sub.material.textureName;
                // If just a filename (no directory), prepend model directory
                if (!modelDir.empty() && texPath.find('/') == std::string::npos &&
                    texPath.find('\\') == std::string::npos &&
                    texPath.find(':') == std::string::npos) {
                    texPath = modelDir + texPath;
                }
            } else if (i < textureNames.size() && !textureNames[i].empty()) {
                texPath = modelDir + textureNames[i];
            }
            if (!texPath.empty()) {
                // Normalize backslashes to forward slashes (WE convention)
                std::replace(texPath.begin(), texPath.end(), '\\', '/');
                // Convert relative paths to absolute (WE resolves from internal root)
                if (texPath.find(':') == std::string::npos && !texPath.empty()) {
                    texPath = wi::helper::GetCurrentPath() + "/" + texPath;
                }
                material->textures[wi::scene::MaterialComponent::BASECOLORMAP].name = texPath;
                // Explicitly pre-load texture into resource manager before CreateRenderData
                // (CreateRenderData queues async load but may not resolve without this)
                if (wi::helper::FileExists(texPath)) {
                    material->textures[wi::scene::MaterialComponent::BASECOLORMAP].resource =
                        wi::resourcemanager::Load(texPath);
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
        for (const auto& v : sub.vertices) {
            mesh->vertex_positions.push_back(DirectX::XMFLOAT3(v.px, v.py, v.pz));
            mesh->vertex_normals.push_back(DirectX::XMFLOAT3(v.nx, v.ny, v.nz));
            mesh->vertex_uvset_0.push_back(DirectX::XMFLOAT2(v.u, v.v));
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
            // Scan model file for texture filenames
            std::vector<std::string> textureNames = bc::scanTextureNames(modelPath);
            // Get model directory for resolving relative texture paths
            std::string modelDir;
            size_t lastSlash = modelPath.find_last_of("/\\");
            if (lastSlash != std::string::npos)
                modelDir = modelPath.substr(0, lastSlash + 1);

            // Count submeshes with per-submesh texture names from Irrlicht
            size_t irrlichtTexCount = 0;
            for (const auto& s : converted.submeshes) {
                if (!s.material.textureName.empty()) irrlichtTexCount++;
            }

            wi::ecs::Entity meshEntity = createWEMeshFromConverted(
                scene, converted, name, textureNames, modelDir, allowTransparency);
            if (meshEntity != wi::ecs::INVALID_ENTITY) {
                g_convertedMeshCache[modelPath] = meshEntity;
                weLog("    Loaded via Irrlicht: " + modelPath +
                      " (" + std::to_string(converted.submeshes.size()) + " submeshes, " +
                      std::to_string(totalVerts) + " verts, " +
                      std::to_string(totalIdx / 3) + " tris, " +
                      std::to_string(irrlichtTexCount) + "/" +
                      std::to_string(converted.submeshes.size()) + " per-submesh textures, " +
                      std::to_string(textureNames.size()) + " scanned)");
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
    bc::gui::ImGuiOverlay overlay;
    overlay.init(width, height);
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
    bc::graphics::wicked::WickedWater ocean;
    float ownShipX = 0, ownShipZ = 0;
    float ownShipHeading = scenarioData.ownShipData.initialBearing;
    float ownShipSpeed = scenarioData.ownShipData.initialSpeed; // knots
    float ownShipRudder = 0; // wheel angle degrees (-30 to 30)
    float ownShipPortEngine = 0; // -1.0 to 1.0
    float ownShipStbdEngine = 0; // -1.0 to 1.0
    float maxSpeedAhead = 14.0f; // knots, read from boat.ini
    float ownShipBowThruster = 0; // -1.0 to 1.0
    float beaufortScale = 3.0f; // sea state for wave heading disturbance
    float ownShipScaleFactor = 1.0f;
    float ownShipHeightCorr = 0;
    float cameraViewX = 0, cameraViewY = 10.0f, cameraViewZ = 0; // bridge position in world coords
    float viewLocalX = 0, viewLocalY = 0, viewLocalZ = 0; // bridge view in ship-local coords (pre-scale)
    wi::ecs::Entity ownShipEntity = wi::ecs::INVALID_ENTITY; // stored to toggle visibility

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

    // Navigation light on another ship (WE emissive point)
    struct WENavLight {
        wi::ecs::Entity entity = wi::ecs::INVALID_ENTITY;
        int shipIndex;           // index into otherShipStates
        float localX, localY, localZ; // position relative to ship model origin
        float r, g, b;          // color (0-1)
        float startAngle, endAngle; // directional arc (degrees)
        float range;             // visibility range (metres)
        std::string sequence;    // flash pattern ('D' = dark)
        float charTime;          // seconds per sequence character
        float timeOffset;        // random phase offset
    };
    std::vector<WENavLight> navLights;

    try { // Wrap scene setup in try-catch to diagnose crashes

    // ===== SUN / LIGHTING =====
    pumpMessages(); // Keep window responsive during setup
    weLog("  Setting up sun/lighting...");
    wi::ecs::Entity sunEntity = scene.Entity_CreateLight("Sun");
    wi::scene::LightComponent* sunLight = scene.lights.GetComponent(sunEntity);
    if (sunLight) {
        sunLight->SetType(wi::scene::LightComponent::DIRECTIONAL);
        sunLight->intensity = 8.0f;
        sunLight->SetCastShadow(true);
    }
    // Position sun based on time of day
    float timeOfDay = scenarioData.startTime; // hours (0-24)
    float sunRise = scenarioData.sunRise > 0 ? scenarioData.sunRise : 6.0f;
    float sunSet = scenarioData.sunSet > 0 ? scenarioData.sunSet : 18.0f;
    float dayLength = sunSet - sunRise;
    float sunProgress = (dayLength > 0) ? (timeOfDay - sunRise) / dayLength : 0.5f;
    sunProgress = std::max(0.0f, std::min(1.0f, sunProgress));
    float sunElevation = -((float)M_PI * sunProgress); // 0=horizon, -PI/2=zenith
    float sunAzimuth = 0.3f; // slightly from south

    wi::scene::TransformComponent* sunTransform = scene.transforms.GetComponent(sunEntity);
    if (sunTransform) {
        sunTransform->RotateRollPitchYaw(DirectX::XMFLOAT3(sunElevation, sunAzimuth, 0.0f));
        sunTransform->UpdateTransform();
    }

    // ===== ATMOSPHERE / WEATHER =====
    scene.weather.SetRealisticSky(true);
    scene.weather.SetVolumetricClouds(true);
    scene.weather.ambient = DirectX::XMFLOAT3(0.3f, 0.35f, 0.4f);

    // Fog from visibility range
    float visRange = scenarioData.visibilityRange;
    if (visRange <= 0) visRange = 10.0f; // default 10 nm
    float fogDistMeters = visRange * 1852.0f; // nm to meters
    scene.weather.fogStart = fogDistMeters * 0.3f;
    // WE uses fogDensity instead of fogEnd; higher density = thicker fog
    if (visRange < 5.0f) {
        scene.weather.fogDensity = 0.01f / std::max(visRange, 0.1f);
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

            nlt.range = IniFile::iniFileTof32(boatIni, IniFile::enumerate1("LightRange", nl));
            nlt.range *= (float)M_IN_NM; // Nm -> metres

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
                lightComp->intensity = 5.0f;
                lightComp->range = 50.0f; // visual glow radius in WE units
                lightComp->SetCastShadow(false);
            }

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
            wi::ecs::Entity buoyEntity = loadModelOrPlaceholder(scene, buoyModelPath,
                                                                 objName, 0.8f, 0.2f, 0.2f, 3.0f);
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
            wi::ecs::Entity loEntity = loadModelOrPlaceholder(scene, loModelPath,
                                                               objName, 0.5f, 0.35f, 0.2f, 8.0f);
            setEntityTransform(scene, loEntity, ox, objY, oz, rotation, loScale);
        }
        weLog("  Loaded " + std::to_string(numLandObjs) + " land objects");
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
    // OwnShip sets axialSpd from InitialSpeed but leaves portEngine/stbdEngine at 0.
    // Compute an initial engine setting from the scenario speed and ship max speed
    // so the ship doesn't immediately decelerate.
    {
        float initSpeed = scenarioData.ownShipData.initialSpeed; // knots
        float engineFraction = (maxSpeedAhead > 0) ? std::min(1.0f, initSpeed / maxSpeedAhead) : 0.0f;
        ownShipPortEngine = engineFraction;
        ownShipStbdEngine = engineFraction;
        SimBridge::setPortEngine(ownShipPortEngine);
        SimBridge::setStbdEngine(ownShipStbdEngine);
    }
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

            // ESC = quit
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
                weLog("ESC pressed -- posting quit");
                PostQuitMessage(0);
                continue;
            }

            // ===== OWN SHIP CONTROLS =====
            // Skip keyboard controls if ImGui wants input or GUI slider is active
            bool imguiWantsKB = bc::graphics::wicked::ImGuiWantsKeyboard();
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

            // Guard against NaN/inf from SimulationModel (propagates to GPU and crashes DX12)
            if (std::isnan(ownShipX) || std::isinf(ownShipX)) { weLogErr("NaN/inf ownShipX"); ownShipX = 0; }
            if (std::isnan(ownShipZ) || std::isinf(ownShipZ)) { weLogErr("NaN/inf ownShipZ"); ownShipZ = 0; }
            if (std::isnan(ownShipHeading) || std::isinf(ownShipHeading)) { weLogErr("NaN/inf heading"); ownShipHeading = 0; }

            float headRad = ownShipHeading * (float)M_PI / 180.0f;
            float ownShipY = SimBridge::getPosY();
            float rawPitch = SimBridge::getPitch();
            float rawRoll = SimBridge::getRoll();

            if (std::isnan(ownShipY) || std::isinf(ownShipY)) { weLogErr("NaN/inf ownShipY"); ownShipY = 0; }
            if (std::isnan(rawPitch) || std::isinf(rawPitch)) { rawPitch = 0; }
            if (std::isnan(rawRoll) || std::isinf(rawRoll)) { rawRoll = 0; }

            // Clamp pitch/roll to reasonable values for visual comfort
            // (large vessels shouldn't pitch/roll more than ~5 degrees in normal seas)
            float ownShipPitch = std::max(-5.0f, std::min(5.0f, rawPitch));
            float ownShipRollAngle = std::max(-8.0f, std::min(8.0f, rawRoll));

            // Update own ship entity position (dynamic Y from wave heave + tide)
            if (ownShipEntity != wi::ecs::INVALID_ENTITY) {
                setEntityTransform(scene, ownShipEntity, ownShipX, ownShipY, ownShipZ,
                                   ownShipHeading, ownShipScaleFactor,
                                   ownShipPitch, ownShipRollAngle);
            }

            // Update bridge camera position (follows own ship including wave motion)
            // ownShipY = heightCorrection + tide + waveHeave; only attenuate dynamic portion
            float camYAttenuation = 0.3f; // 30% of wave/tide heave to camera
            if (!camOrbitMode) {
                float vxScaled = viewLocalX * ownShipScaleFactor;
                float vzScaled = viewLocalZ * ownShipScaleFactor;
                camPosX = ownShipX + vxScaled * std::cos(headRad) + vzScaled * std::sin(headRad);
                float dynamicY = ownShipY - ownShipHeightCorr; // tide + wave only
                camPosY = ownShipHeightCorr + dynamicY * camYAttenuation + viewLocalY * ownShipScaleFactor;
                camPosZ = ownShipZ - vxScaled * std::sin(headRad) + vzScaled * std::cos(headRad);
                // Clamp camera above water surface to prevent underwater rendering issues
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
                }
            }

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

            // ===== NAVIGATION LIGHTS (on other ships) =====
            {
                float scenarioTime = SimBridge::getTimeDelta();
                uint32_t lightLevel = SimBridge::getLightLevel();
                float lightAlpha = (255.0f - (float)lightLevel) / 255.0f; // 0=invisible(day), 1=bright(night)

                for (auto& nlt : navLights) {
                    if (nlt.entity == wi::ecs::INVALID_ENTITY) continue;
                    if (nlt.shipIndex < 0 || nlt.shipIndex >= (int)otherShipStates.size()) continue;

                    const auto& ship = otherShipStates[nlt.shipIndex];
                    float sf = ship.scaleFactor;
                    float headRad = ship.heading * (float)M_PI / 180.0f;
                    float cosH = std::cos(headRad), sinH = std::sin(headRad);

                    // Transform local light position to world space
                    float wx = ship.x + (nlt.localX * cosH + nlt.localZ * sinH) * sf;
                    float wy = ship.heightCorr + nlt.localY * sf;
                    float wz = ship.z + (-nlt.localX * sinH + nlt.localZ * cosH) * sf;

                    // Check visibility: range
                    float dx = wx - camPosX, dz = wz - camPosZ, dy = wy - camPosY;
                    float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
                    bool visible = (dist <= nlt.range) && (lightAlpha > 0.05f);

                    // Check visibility: directional arc
                    if (visible) {
                        // Angle from light to camera in world coords
                        float angleToCamera = std::atan2(dx, dz) * 180.0f / (float)M_PI; // degrees
                        // Convert to angle relative to ship heading
                        float localAngle = angleToCamera - ship.heading;
                        // Normalize to 0-360
                        while (localAngle < 0) localAngle += 360;
                        while (localAngle >= 360) localAngle -= 360;
                        // Check if within arc
                        float sa = nlt.startAngle, ea = nlt.endAngle;
                        while (sa < 0) { sa += 360; ea += 360; }
                        while (sa >= 360) { sa -= 360; ea -= 360; }
                        if (ea <= 360) {
                            visible = (localAngle >= sa && localAngle <= ea);
                        } else {
                            float normEnd = ea;
                            while (normEnd >= 360) normEnd -= 360;
                            visible = (localAngle >= sa || localAngle <= normEnd);
                        }
                    }

                    // Check visibility: flash sequence
                    if (visible && !nlt.sequence.empty()) {
                        size_t seqLen = nlt.sequence.length();
                        float timeInSeq = std::fmod((scenarioTime + nlt.timeOffset) / nlt.charTime, (float)seqLen);
                        size_t pos = (size_t)timeInSeq;
                        if (pos >= seqLen) pos = seqLen - 1;
                        if (nlt.sequence[pos] == 'D' || nlt.sequence[pos] == 'd')
                            visible = false;
                    }

                    // Position the point light
                    setEntityTransform(scene, nlt.entity, wx, wy, wz);

                    // Control intensity: bright at night, off during day
                    auto* lightComp = scene.lights.GetComponent(nlt.entity);
                    if (lightComp) {
                        lightComp->intensity = visible ? (lightAlpha * 8.0f) : 0.0f;
                    }
                }
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
                // Bridge first-person mode
                // Camera position follows own ship (updated above in ship controls)
                camX = camPosX;
                camY = camPosY;
                camZ = camPosZ;

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
                // Feed input state to ImGui
                ImGuiIO& io = ImGui::GetIO();
                io.DisplaySize = ImVec2((float)width, (float)height);
                io.DeltaTime = dt > 0 ? dt : 1.0f / 60.0f;
                io.MousePos = ImVec2((float)mousePos.x, (float)mousePos.y);
                io.MouseDown[0] = lbDown;
                io.MouseDown[1] = rbDown;

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
                overlay.render();

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
                bool fileRequest = Utilities::pathExists(screenshotRequestPath);

                if ((f12IsDown && !f12WasDown) || fileRequest) {
                    std::string outPath = fileRequest ? screenshotOutputPath : "";
                    std::string result = wi::helper::screenshot(application.swapChain, outPath);
                    if (!result.empty()) {
                        weLog("Screenshot saved: " + result);
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
