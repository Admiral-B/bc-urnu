/*   Bridge Command 5.0 Ship Simulator
     Copyright (C) 2026 James Packer

     This program is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License version 2 as
     published by the Free Software Foundation

     OBJ import adapted from Wicked Engine Editor (MIT license)
     PLY/3DGS import adapted from Wicked Engine Editor (MIT license)
     Original: Copyright (c) Turanszki Janos
     miniply: Copyright (c) 2019 Vilya Harvey (MIT license) */

#ifdef WITH_WICKED_ENGINE

#include "WickedModelImporter.hpp"

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include "miniply.h"

#include <fstream>
#include <iostream>
#include <algorithm>
#include <unordered_map>

using namespace wi::graphics;
using namespace wi::scene;
using namespace wi::ecs;

namespace bc { namespace graphics { namespace wicked {

// Helper: get directory from path
static std::string getDirectory(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    if (pos != std::string::npos) return path.substr(0, pos + 1);
    return "";
}

// Helper: get filename from path
static std::string getFilename(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    if (pos != std::string::npos) return path.substr(pos + 1);
    return path;
}

// Helper: get file extension (lowercase)
static std::string getExtension(const std::string& path) {
    size_t pos = path.find_last_of('.');
    if (pos != std::string::npos) {
        std::string ext = path.substr(pos);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        return ext;
    }
    return "";
}

// Read a file into a byte vector
static bool readFile(const std::string& path, std::vector<uint8_t>& data) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return false;
    auto size = file.tellg();
    if (size <= 0) return false;
    data.resize(static_cast<size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(data.data()), size);
    return true;
}

// membuf for streaming file data
struct membuf : std::streambuf {
    membuf(char* begin, char* end) {
        this->setg(begin, begin, end);
    }
};

// Custom material file reader for TinyObjLoader
class MaterialFileReader : public tinyobj::MaterialReader {
public:
    explicit MaterialFileReader(const std::string& mtlDir) : dir_(mtlDir) {}
    ~MaterialFileReader() override = default;

    bool operator()(const std::string& matId,
                    std::vector<tinyobj::material_t>* materials,
                    std::map<std::string, int>* matMap,
                    std::string* err) override {
        std::string filepath = dir_.empty() ? matId : dir_ + matId;
        std::vector<uint8_t> filedata;
        if (!readFile(filepath, filedata)) {
            if (err) *err += "WARN: Material file [ " + filepath + " ] not found.\n";
            return false;
        }
        membuf sbuf(reinterpret_cast<char*>(filedata.data()),
                     reinterpret_cast<char*>(filedata.data() + filedata.size()));
        std::istream in(&sbuf);
        std::string warning;
        LoadMtl(matMap, materials, &in, &warning);
        if (!warning.empty() && err) *err += warning;
        return true;
    }

private:
    std::string dir_;
};

// Hash for unique vertex deduplication
struct IndexAndMat {
    tinyobj::index_t index;
    int materialIndex;
    bool operator==(const IndexAndMat& other) const {
        return index.vertex_index == other.index.vertex_index &&
               index.normal_index == other.index.normal_index &&
               index.texcoord_index == other.index.texcoord_index &&
               materialIndex == other.materialIndex;
    }
};

struct IndexAndMatHash {
    size_t operator()(const IndexAndMat& o) const noexcept {
        size_t h = 0;
        h ^= std::hash<int>()(o.index.vertex_index) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>()(o.index.normal_index) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>()(o.index.texcoord_index) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>()(o.materialIndex) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

void ImportModel_OBJ(const std::string& filename, Scene& scene,
                      Entity rootEntity) {
    std::string directory = getDirectory(filename);
    std::string name = getFilename(filename);

    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string errors;

    std::vector<uint8_t> filedata;
    bool success = readFile(filename, filedata);

    if (success) {
        membuf sbuf(reinterpret_cast<char*>(filedata.data()),
                     reinterpret_cast<char*>(filedata.data() + filedata.size()));
        std::istream in(&sbuf);
        MaterialFileReader matReader(directory);
        success = tinyobj::LoadObj(&attrib, &shapes, &materials, &errors, &in, &matReader, true);
    } else {
        errors = "Failed to read file: " + filename;
    }

    if (!errors.empty()) {
        std::cerr << "WickedModelImporter OBJ: " << errors << std::endl;
    }

    if (!success) return;

    // Load material library
    std::vector<Entity> materialLibrary;
    for (auto& mat : materials) {
        Entity matEntity = scene.Entity_CreateMaterial(mat.name);
        scene.Component_Attach(matEntity, rootEntity);
        MaterialComponent& material = *scene.materials.GetComponent(matEntity);

        material.baseColor = DirectX::XMFLOAT4(mat.diffuse[0], mat.diffuse[1], mat.diffuse[2], 1.0f);
        material.textures[MaterialComponent::BASECOLORMAP].name = mat.diffuse_texname;
        material.textures[MaterialComponent::DISPLACEMENTMAP].name = mat.displacement_texname;
        material.emissiveColor.x = mat.emission[0];
        material.emissiveColor.y = mat.emission[1];
        material.emissiveColor.z = mat.emission[2];
        material.emissiveColor.w = std::max(mat.emission[0], std::max(mat.emission[1], mat.emission[2]));
        material.metalness = mat.metallic;
        material.textures[MaterialComponent::NORMALMAP].name = mat.normal_texname;
        material.textures[MaterialComponent::SURFACEMAP].name = mat.specular_texname;
        material.roughness = mat.roughness;

        if (material.textures[MaterialComponent::NORMALMAP].name.empty())
            material.textures[MaterialComponent::NORMALMAP].name = mat.bump_texname;
        if (material.textures[MaterialComponent::SURFACEMAP].name.empty())
            material.textures[MaterialComponent::SURFACEMAP].name = mat.specular_highlight_texname;

        // TinyObjLoader PBR extensions: map_Pr (roughness), map_Pm (metallic)
        if (material.textures[MaterialComponent::SURFACEMAP].name.empty() && !mat.roughness_texname.empty())
            material.textures[MaterialComponent::SURFACEMAP].name = mat.roughness_texname;
        if (material.textures[MaterialComponent::SURFACEMAP].name.empty() && !mat.metallic_texname.empty())
            material.textures[MaterialComponent::SURFACEMAP].name = mat.metallic_texname;

        // Resolve texture paths relative to model directory
        for (auto& tex : material.textures) {
            if (!tex.name.empty()) {
                tex.name = directory + tex.name;
            }
        }

        // Auto-detect PBR maps alongside base texture if not explicitly set
        // Convention: wall.png -> wall_Normal.png, wall_Roughness.png
        std::string baseTexPath = material.textures[MaterialComponent::BASECOLORMAP].name;
        if (!baseTexPath.empty()) {
            size_t dotPos = baseTexPath.rfind('.');
            if (dotPos != std::string::npos) {
                std::string stem = baseTexPath.substr(0, dotPos);
                std::string ext = baseTexPath.substr(dotPos);

                if (material.textures[MaterialComponent::NORMALMAP].name.empty()) {
                    std::string normalPath = stem + "_Normal" + ext;
                    if (wi::helper::FileExists(normalPath)) {
                        material.textures[MaterialComponent::NORMALMAP].name = normalPath;
                    }
                }
                if (material.textures[MaterialComponent::SURFACEMAP].name.empty()) {
                    std::string roughPath = stem + "_Roughness" + ext;
                    if (wi::helper::FileExists(roughPath)) {
                        material.textures[MaterialComponent::SURFACEMAP].name = roughPath;
                    }
                }
            }
        }

        material.CreateRenderData();
        materialLibrary.push_back(matEntity);
    }

    // Create default material if none found
    if (materialLibrary.empty()) {
        Entity matEntity = scene.Entity_CreateMaterial("BC_DefaultMaterial");
        scene.Component_Attach(matEntity, rootEntity);
        materialLibrary.push_back(matEntity);
    }

    // Load shapes (objects/meshes)
    for (auto& shape : shapes) {
        Entity objectEntity = scene.Entity_CreateObject(shape.name);
        scene.Component_Attach(objectEntity, rootEntity);
        Entity meshEntity = scene.Entity_CreateMesh(shape.name + "_mesh");
        scene.Component_Attach(meshEntity, rootEntity);
        ObjectComponent& object = *scene.objects.GetComponent(objectEntity);
        MeshComponent& mesh = *scene.meshes.GetComponent(meshEntity);

        object.meshID = meshEntity;

        std::unordered_map<int, int> registeredMaterials;
        std::unordered_map<IndexAndMat, uint32_t, IndexAndMatHash> uniqueVertices;

        for (size_t i = 0; i < shape.mesh.indices.size(); i += 3) {
            tinyobj::index_t indices[3] = {
                shape.mesh.indices[i + 0],
                shape.mesh.indices[i + 1],
                shape.mesh.indices[i + 2],
            };

            for (auto& index : indices) {
                DirectX::XMFLOAT3 pos(
                    attrib.vertices[index.vertex_index * 3 + 0],
                    attrib.vertices[index.vertex_index * 3 + 1],
                    attrib.vertices[index.vertex_index * 3 + 2]
                );

                DirectX::XMFLOAT3 nor(0, 0, 0);
                if (!attrib.normals.empty() && index.normal_index >= 0) {
                    nor = DirectX::XMFLOAT3(
                        attrib.normals[index.normal_index * 3 + 0],
                        attrib.normals[index.normal_index * 3 + 1],
                        attrib.normals[index.normal_index * 3 + 2]
                    );
                }

                DirectX::XMFLOAT2 tex(0, 0);
                if (index.texcoord_index >= 0 && !attrib.texcoords.empty()) {
                    tex = DirectX::XMFLOAT2(
                        attrib.texcoords[index.texcoord_index * 2 + 0],
                        1.0f - attrib.texcoords[index.texcoord_index * 2 + 1]
                    );
                }

                int matIdx = std::max(0, shape.mesh.material_ids[i / 3]);
                if (registeredMaterials.count(matIdx) == 0) {
                    registeredMaterials[matIdx] = static_cast<int>(mesh.subsets.size());
                    mesh.subsets.push_back(MeshComponent::MeshSubset());
                    mesh.subsets.back().materialID = materialLibrary[std::min(matIdx, (int)materialLibrary.size() - 1)];
                    mesh.subsets.back().indexOffset = static_cast<uint32_t>(mesh.indices.size());
                }

                // OBJ is right-handed, WE is left-handed: flip Z
                pos.z *= -1;
                nor.z *= -1;

                IndexAndMat im = {index, matIdx};
                if (uniqueVertices.count(im) == 0) {
                    uniqueVertices[im] = static_cast<uint32_t>(mesh.vertex_positions.size());
                    mesh.vertex_positions.push_back(pos);
                    mesh.vertex_normals.push_back(nor);
                    mesh.vertex_uvset_0.push_back(tex);
                }
                mesh.indices.push_back(uniqueVertices[im]);
                mesh.subsets.back().indexCount++;
            }
        }
        mesh.CreateRenderData();
    }
}

// PLY import -- supports both regular meshes and 3D Gaussian Splat models.
// Adapted from WE Editor's ModelImporter_PLY.cpp.
static void ImportModel_PLY(const std::string& fileName, Scene& scene) {
    miniply::PLYReader reader(fileName.c_str());
    if (!reader.valid()) {
        std::cerr << "PLY file invalid: " << fileName << std::endl;
        return;
    }

    uint32_t propIndices[64] = {};
    bool gotVerts = false;
    bool gotFaces = false;

    wi::vector<uint32_t> indices;
    wi::vector<XMFLOAT3> positions;
    wi::vector<XMFLOAT3> normals;
    wi::vector<XMFLOAT2> texcoords;

    // Gaussian splat fields
    wi::vector<XMFLOAT4> rotations;
    wi::vector<XMFLOAT3> scales;
    wi::vector<float> opacities;
    wi::vector<XMFLOAT3> f_dc;
    wi::vector<float> f_rest;

    while (reader.has_element() && (!gotVerts || !gotFaces)) {
        if (reader.element_is(miniply::kPLYVertexElement) && reader.load_element() && reader.find_pos(propIndices)) {
            const uint32_t vertexCount = reader.num_rows();
            positions.resize(vertexCount);
            reader.extract_properties(propIndices, 3, miniply::PLYPropertyType::Float, positions.data());
            if (reader.find_normal(propIndices)) {
                normals.resize(vertexCount);
                reader.extract_properties(propIndices, 3, miniply::PLYPropertyType::Float, normals.data());
            }
            if (reader.find_texcoord(propIndices)) {
                texcoords.resize(vertexCount);
                reader.extract_properties(propIndices, 2, miniply::PLYPropertyType::Float, texcoords.data());
            }
            if (reader.find_properties(propIndices, 4, "rot_0", "rot_1", "rot_2", "rot_3")) {
                rotations.resize(vertexCount);
                reader.extract_properties(propIndices, 4, miniply::PLYPropertyType::Float, rotations.data());
            }
            if (reader.find_properties(propIndices, 3, "scale_0", "scale_1", "scale_2")) {
                scales.resize(vertexCount);
                reader.extract_properties(propIndices, 3, miniply::PLYPropertyType::Float, scales.data());
            }
            if (reader.find_properties(propIndices, 1, "opacity")) {
                opacities.resize(vertexCount);
                reader.extract_properties(propIndices, 1, miniply::PLYPropertyType::Float, opacities.data());
            }
            if (reader.find_properties(propIndices, 3, "f_dc_0", "f_dc_1", "f_dc_2")) {
                f_dc.resize(vertexCount);
                reader.extract_properties(propIndices, 3, miniply::PLYPropertyType::Float, f_dc.data());
            }
            if (reader.find_properties(propIndices, 45,
                "f_rest_0", "f_rest_1", "f_rest_2", "f_rest_3", "f_rest_4", "f_rest_5",
                "f_rest_6", "f_rest_7", "f_rest_8", "f_rest_9", "f_rest_10", "f_rest_11", "f_rest_12",
                "f_rest_13", "f_rest_14", "f_rest_15", "f_rest_16", "f_rest_17", "f_rest_18",
                "f_rest_19", "f_rest_20", "f_rest_21", "f_rest_22", "f_rest_23", "f_rest_24",
                "f_rest_25", "f_rest_26", "f_rest_27", "f_rest_28", "f_rest_29", "f_rest_30", "f_rest_31",
                "f_rest_32", "f_rest_33", "f_rest_34", "f_rest_35", "f_rest_36", "f_rest_37", "f_rest_38",
                "f_rest_39", "f_rest_40", "f_rest_41", "f_rest_42", "f_rest_43", "f_rest_44")) {
                f_rest.resize(vertexCount * 45);
                reader.extract_properties(propIndices, 45, miniply::PLYPropertyType::Float, f_rest.data());
            }
            gotVerts = true;
        } else if (reader.element_is(miniply::kPLYFaceElement) && reader.load_element() && reader.find_indices(propIndices)) {
            bool polys = reader.requires_triangulation(propIndices[0]);
            if (polys && !gotVerts) {
                std::cerr << "PLY: need vertex positions to triangulate faces" << std::endl;
                break;
            }
            if (polys) {
                indices.resize(reader.num_triangles(propIndices[0]) * 3);
                reader.extract_triangles(propIndices[0], (float*)positions.data(), (uint32_t)positions.size(), miniply::PLYPropertyType::Int, indices.data());
            } else {
                indices.resize(reader.num_rows() * 3);
                reader.extract_list_property(propIndices[0], miniply::PLYPropertyType::Int, indices.data());
            }
            gotFaces = true;
        }
        if (gotVerts && gotFaces) break;
        reader.next_element();
    }

    if (positions.empty()) return;

    Entity entity = CreateEntity();
    scene.names.Create(entity) = getFilename(fileName);
    scene.transforms.Create(entity);

    if (!f_rest.empty()) {
        // 3D Gaussian Splat model
        wi::GaussianSplatModel& splat = scene.gaussian_splats.Create(entity);
        // Flip Y axis (standard for gaussian splat PLY files)
        for (auto& x : positions) x.y *= -1;
        for (auto& x : rotations) {
            x = XMFLOAT4(x.y, x.z, x.w, x.x);
            x.x = -x.x;
            x.z = -x.z;
            XMVECTOR Q = XMLoadFloat4(&x);
            Q = XMQuaternionNormalize(Q);
            XMStoreFloat4(&x, Q);
        }
        splat.positions = std::move(positions);
        splat.rotations = std::move(rotations);
        splat.scales = std::move(scales);
        splat.opacities = std::move(opacities);
        splat.f_dc = std::move(f_dc);
        splat.f_rest = std::move(f_rest);
        splat.CreateRenderData();
        std::cout << "PLY: Loaded 3DGS model with " << splat.GetSplatCount() << " splats" << std::endl;
    } else {
        // Regular mesh
        MeshComponent& mesh = scene.meshes.Create(entity);
        mesh.indices = std::move(indices);
        mesh.vertex_positions = std::move(positions);
        mesh.vertex_normals = std::move(normals);
        mesh.vertex_uvset_0 = std::move(texcoords);
        MeshComponent::MeshSubset& subset = mesh.subsets.emplace_back();
        subset.materialID = entity;
        subset.indexCount = (uint32_t)mesh.indices.size();
        if (subset.indexCount == 0) {
            size_t vertexCount = mesh.vertex_positions.size() / 3 * 3;
            mesh.indices.resize(vertexCount);
            for (size_t vi = 0; vi < vertexCount; vi += 3) {
                mesh.indices[vi + 0] = uint32_t(vi);
                mesh.indices[vi + 1] = uint32_t(vi);
                mesh.indices[vi + 2] = uint32_t(vi);
            }
            subset.indexCount = (uint32_t)vertexCount;
        }
        mesh.FlipCulling();
        if (mesh.vertex_normals.empty()) {
            mesh.ComputeNormals(MeshComponent::COMPUTE_NORMALS_HARD);
        }
        scene.materials.Create(entity);
        ObjectComponent& object = scene.objects.Create(entity);
        object.meshID = entity;
        mesh.CreateRenderData();
        std::cout << "PLY: Loaded mesh with " << mesh.vertex_positions.size() << " vertices" << std::endl;
    }
}

wi::ecs::Entity LoadModelFromFile(const std::string& filename, Scene& scene) {
    std::string ext = getExtension(filename);

    // Create root entity for the model
    Entity rootEntity = CreateEntity();
    scene.transforms.Create(rootEntity);
    scene.names.Create(rootEntity) = getFilename(filename);

    if (ext == ".obj") {
        ImportModel_OBJ(filename, scene, rootEntity);
    } else if (ext == ".wiscene") {
        // Native WE format -- load via WE's built-in loader
        // LoadModel merges into the global scene; for our purposes, use LoadModel2
        scene.Entity_Remove(rootEntity); // Remove our placeholder
        rootEntity = wi::scene::LoadModel(filename);
    } else if (ext == ".gltf" || ext == ".glb") {
        // glTF/GLB import via ported WE Editor importer (uses tinygltf)
        // Imports into a temp scene internally, FlipZAxis there, then merges.
        // Returns the root entity (ID preserved across merge).
        scene.Entity_Remove(rootEntity);
        rootEntity = ImportModel_GLTF(filename, scene);
    } else if (ext == ".ply") {
        // PLY import -- supports regular meshes and 3D Gaussian Splat models
        scene.Entity_Remove(rootEntity);
        ImportModel_PLY(filename, scene);
        // ImportModel_PLY creates its own entity; return it
        // (last entity in gaussian_splats or objects)
        if (scene.gaussian_splats.GetCount() > 0) {
            rootEntity = scene.gaussian_splats.GetEntity(scene.gaussian_splats.GetCount() - 1);
        } else if (scene.objects.GetCount() > 0) {
            rootEntity = scene.objects.GetEntity(scene.objects.GetCount() - 1);
        } else {
            rootEntity = INVALID_ENTITY;
        }
    } else if (ext == ".x" || ext == ".3ds") {
        // DirectX .x and .3ds formats are not supported by Wicked Engine.
        // These models need to be converted to .obj or .gltf first.
        std::cerr << "WickedModelImporter: Unsupported format '" << ext
                  << "' for file: " << filename << std::endl;
        scene.Entity_Remove(rootEntity);
        return INVALID_ENTITY;
    } else {
        std::cerr << "WickedModelImporter: Unknown format: " << ext << std::endl;
        scene.Entity_Remove(rootEntity);
        return INVALID_ENTITY;
    }

    return rootEntity;
}

}}} // namespace bc::graphics::wicked

#endif // WITH_WICKED_ENGINE
