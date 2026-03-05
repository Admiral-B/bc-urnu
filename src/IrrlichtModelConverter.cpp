/*   Bridge Command 5.0 Ship Simulator
     Copyright (C) 2026 James Packer

     This program is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License version 2 as
     published by the Free Software Foundation

     Irrlicht-based model converter: loads .x, .3ds, and other legacy model
     formats using a headless Irrlicht device, extracting vertex data for use
     with Wicked Engine's renderer. */

// IMPORTANT: This file must NOT include WickedEngine.h or any WE headers.
// It is a separate compilation unit that only uses Irrlicht.

#include "IrrlichtModelConverter.hpp"
#include "irrlicht.h"
#include "ISkinnedMesh.h"
#include "SSkinMeshBuffer.h"

#include <iostream>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace bc {

static irr::IrrlichtDevice* g_converterDevice = nullptr;
static irr::scene::ISceneManager* g_converterSmgr = nullptr;

static bool ensureDevice() {
    if (g_converterDevice) return true;

    // EDT_NULL is sufficient: Irrlicht's .x/.3ds loaders parse materials and
    // create SDummyTexture objects with correct filenames even with the NULL driver.
    // This gives us per-submesh texture names without needing a real renderer.
    irr::SIrrlichtCreationParameters params;
    params.DriverType = irr::video::EDT_NULL;
    params.WindowSize = irr::core::dimension2d<irr::u32>(1, 1);
    params.Stencilbuffer = false;
    params.AntiAlias = 0;
    params.WithAlphaChannel = false;

    g_converterDevice = irr::createDeviceEx(params);
    if (!g_converterDevice) {
        std::cerr << "IrrlichtModelConverter: Failed to create EDT_NULL device" << std::endl;
        return false;
    }

    g_converterSmgr = g_converterDevice->getSceneManager();
    return g_converterSmgr != nullptr;
}

ConvertedModel convertModelViaIrrlicht(const std::string& filepath) {
    ConvertedModel result;
    if (!ensureDevice()) return result;

    // Extract model directory and filename
    std::string modelDir;
    std::string modelFileName = filepath;
    size_t lastSlash = filepath.find_last_of("/\\");
    if (lastSlash != std::string::npos) {
        modelDir = filepath.substr(0, lastSlash + 1);
        modelFileName = filepath.substr(lastSlash + 1);
    }

    // Change CWD to model directory so EDT_BURNINGSVIDEO can find textures.
    // IMPORTANT: Irrlicht's changeWorkingDirectoryTo calls SetCurrentDirectory on
    // Windows, which changes the OS process CWD. We MUST save and restore it,
    // otherwise all subsequent relative path operations in the app will break.
    irr::io::IFileSystem* fs = g_converterDevice->getFileSystem();
    irr::io::path savedCwd = fs->getWorkingDirectory();

    if (!modelDir.empty()) {
        fs->changeWorkingDirectoryTo(modelDir.c_str());
    }

    // Load mesh using just the filename (CWD is now the model directory)
    irr::scene::IAnimatedMesh* animMesh = g_converterSmgr->getMesh(modelFileName.c_str());

    // Restore original CWD immediately
    fs->changeWorkingDirectoryTo(savedCwd.c_str());
    if (!animMesh) return result;

    irr::scene::IMesh* mesh = animMesh->getMesh(0);
    if (!mesh) return result;

    // Build a pointer-to-transform lookup for SSkinMeshBuffer transforms.
    // For skinned meshes (.x files), ISkinnedMesh::getMeshBuffers() gives us
    // direct SSkinMeshBuffer* pointers (no dynamic_cast across DLL boundary).
    // We match them to IMeshBuffer* by pointer address.
    std::unordered_map<const irr::scene::IMeshBuffer*, const irr::core::matrix4*> bufferTransforms;
    if (animMesh->getMeshType() == irr::scene::EAMT_SKINNED) {
        auto* skinMesh = static_cast<irr::scene::ISkinnedMesh*>(animMesh);
        auto& skinBuffers = skinMesh->getMeshBuffers();
        for (irr::u32 i = 0; i < skinBuffers.size(); i++) {
            irr::scene::SSkinMeshBuffer* sb = skinBuffers[i];
            if (sb && !sb->Transformation.isIdentity()) {
                bufferTransforms[static_cast<const irr::scene::IMeshBuffer*>(sb)] = &sb->Transformation;
            }
        }
    }

    for (irr::u32 b = 0; b < mesh->getMeshBufferCount(); b++) {
        irr::scene::IMeshBuffer* mb = mesh->getMeshBuffer(b);
        if (!mb || mb->getVertexCount() == 0) continue;

        ConvertedSubMesh submesh;

        // Extract material colors
        const irr::video::SMaterial& mat = mb->getMaterial();
        irr::video::SColor dc = mat.DiffuseColor;
        submesh.material.r = dc.getRed() / 255.0f;
        submesh.material.g = dc.getGreen() / 255.0f;
        submesh.material.b = dc.getBlue() / 255.0f;
        submesh.material.a = dc.getAlpha() / 255.0f;

        irr::video::SColor ec = mat.EmissiveColor;
        submesh.material.er = ec.getRed() / 255.0f;
        submesh.material.eg = ec.getGreen() / 255.0f;
        submesh.material.eb = ec.getBlue() / 255.0f;

        submesh.material.shininess = mat.Shininess;

        // Texture name from Irrlicht material (EDT_NULL creates SDummyTexture with filename)
        if (mat.TextureLayer[0].Texture) {
            submesh.material.textureName =
                mat.TextureLayer[0].Texture->getName().getPath().c_str();
        }

        // Extract vertices
        irr::u32 vertCount = mb->getVertexCount();
        submesh.vertices.resize(vertCount);

        switch (mb->getVertexType()) {
        case irr::video::EVT_STANDARD: {
            auto* v = static_cast<const irr::video::S3DVertex*>(mb->getVertices());
            for (irr::u32 i = 0; i < vertCount; i++) {
                submesh.vertices[i] = {
                    v[i].Pos.X, v[i].Pos.Y, v[i].Pos.Z,
                    v[i].Normal.X, v[i].Normal.Y, v[i].Normal.Z,
                    v[i].TCoords.X, v[i].TCoords.Y
                };
            }
            break;
        }
        case irr::video::EVT_2TCOORDS: {
            auto* v = static_cast<const irr::video::S3DVertex2TCoords*>(mb->getVertices());
            for (irr::u32 i = 0; i < vertCount; i++) {
                submesh.vertices[i] = {
                    v[i].Pos.X, v[i].Pos.Y, v[i].Pos.Z,
                    v[i].Normal.X, v[i].Normal.Y, v[i].Normal.Z,
                    v[i].TCoords.X, v[i].TCoords.Y
                };
            }
            break;
        }
        case irr::video::EVT_TANGENTS: {
            auto* v = static_cast<const irr::video::S3DVertexTangents*>(mb->getVertices());
            for (irr::u32 i = 0; i < vertCount; i++) {
                submesh.vertices[i] = {
                    v[i].Pos.X, v[i].Pos.Y, v[i].Pos.Z,
                    v[i].Normal.X, v[i].Normal.Y, v[i].Normal.Z,
                    v[i].TCoords.X, v[i].TCoords.Y
                };
            }
            break;
        }
        }

        // Apply frame hierarchy transform (SSkinMeshBuffer from .x files).
        // For static .x models, skinMesh() never runs because HasAnimation=false,
        // so vertices are in frame-local coordinates. finalize() stores the
        // global frame transform in SSkinMeshBuffer::Transformation.
        auto txIt = bufferTransforms.find(mb);
        if (txIt != bufferTransforms.end()) {
            const irr::core::matrix4& xform = *txIt->second;
            for (irr::u32 i = 0; i < vertCount; i++) {
                irr::core::vector3df pos(submesh.vertices[i].px,
                                          submesh.vertices[i].py,
                                          submesh.vertices[i].pz);
                irr::core::vector3df norm(submesh.vertices[i].nx,
                                           submesh.vertices[i].ny,
                                           submesh.vertices[i].nz);
                xform.transformVect(pos);
                xform.rotateVect(norm);
                submesh.vertices[i].px = pos.X;
                submesh.vertices[i].py = pos.Y;
                submesh.vertices[i].pz = pos.Z;
                submesh.vertices[i].nx = norm.X;
                submesh.vertices[i].ny = norm.Y;
                submesh.vertices[i].nz = norm.Z;
            }
        }

        // Extract indices (handle both 16-bit and 32-bit index types)
        irr::u32 idxCount = mb->getIndexCount();
        submesh.indices.resize(idxCount);
        if (mb->getIndexType() == irr::video::EIT_32BIT) {
            const irr::u32* idx32 = reinterpret_cast<const irr::u32*>(mb->getIndices());
            for (irr::u32 i = 0; i < idxCount; i++) {
                submesh.indices[i] = static_cast<uint32_t>(idx32[i]);
            }
        } else {
            const irr::u16* idx16 = reinterpret_cast<const irr::u16*>(mb->getIndices());
            for (irr::u32 i = 0; i < idxCount; i++) {
                submesh.indices[i] = static_cast<uint32_t>(idx16[i]);
            }
        }

        result.submeshes.push_back(std::move(submesh));
    }

    result.valid = !result.submeshes.empty();

    // Remove from Irrlicht's mesh cache so the same filename (e.g. "boat.x")
    // in a different directory doesn't return this cached mesh on the next call.
    irr::scene::IMeshCache* cache = g_converterSmgr->getMeshCache();
    if (cache) cache->removeMesh(animMesh);

    return result;
}

void shutdownIrrlichtConverter() {
    if (g_converterDevice) {
        g_converterDevice->drop();
        g_converterDevice = nullptr;
        g_converterSmgr = nullptr;
    }
}

// Scan binary model files for embedded texture filenames by looking for
// strings ending in common image extensions (.jpg, .bmp, .png, .tga, .dds).
// Returns filenames in order of first appearance (deduplicated).
std::vector<std::string> scanTextureNames(const std::string& filepath) {
    std::vector<std::string> result;

    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return result;

    auto fileSize = file.tellg();
    if (fileSize <= 0 || fileSize > 50 * 1024 * 1024) return result; // skip huge files
    file.seekg(0);

    std::vector<char> data(static_cast<size_t>(fileSize));
    file.read(data.data(), fileSize);

    const char* extensions[] = {".jpg", ".bmp", ".png", ".tga", ".dds", ".jpeg"};

    for (size_t i = 0; i < data.size(); i++) {
        for (const char* ext : extensions) {
            size_t extLen = std::strlen(ext);
            if (i + extLen > data.size()) continue;

            // Case-insensitive extension match
            bool match = true;
            for (size_t j = 0; j < extLen; j++) {
                if (std::tolower(static_cast<unsigned char>(data[i + j])) != ext[j]) {
                    match = false;
                    break;
                }
            }
            if (!match) continue;

            // Scan backwards for the start of the filename
            // (printable ASCII, no path separators at start, no control chars)
            size_t nameEnd = i + extLen;
            size_t start = i;
            while (start > 0) {
                char ch = data[start - 1];
                if (ch < 32 || ch > 126) break;      // non-printable
                if (ch == ';' || ch == '"') break;     // delimiter
                if (ch == '{' || ch == '}') break;     // structure
                if (ch == '\0') break;
                start--;
            }

            if (start < nameEnd && (nameEnd - start) > extLen) {
                std::string texName(data.data() + start, data.data() + nameEnd);
                // Skip if it looks like a path (contains unusual chars) or is too long
                if (texName.size() < 200) {
                    // Deduplicate
                    bool exists = false;
                    for (const auto& existing : result) {
                        if (existing == texName) { exists = true; break; }
                    }
                    if (!exists) result.push_back(texName);
                }
            }
            break; // only match first extension per position
        }
    }

    return result;
}

} // namespace bc
