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

#include <iostream>
#include <fstream>
#include <algorithm>
#include <cctype>

namespace bc {

static irr::IrrlichtDevice* g_converterDevice = nullptr;
static irr::scene::ISceneManager* g_converterSmgr = nullptr;

static bool ensureDevice() {
    if (g_converterDevice) return true;

    // Try EDT_BURNINGSVIDEO first (software renderer that actually loads textures,
    // giving us per-submesh texture filenames). Fall back to EDT_NULL if it fails.
    irr::SIrrlichtCreationParameters params;
    params.DriverType = irr::video::EDT_BURNINGSVIDEO;
    params.WindowSize = irr::core::dimension2d<irr::u32>(1, 1);
    params.Stencilbuffer = false;
    params.AntiAlias = 0;
    params.WithAlphaChannel = false;

    g_converterDevice = irr::createDeviceEx(params);
    if (!g_converterDevice) {
        // Fall back to EDT_NULL (no texture name extraction)
        std::cerr << "IrrlichtModelConverter: EDT_BURNINGSVIDEO failed, falling back to EDT_NULL" << std::endl;
        params.DriverType = irr::video::EDT_NULL;
        g_converterDevice = irr::createDeviceEx(params);
        if (!g_converterDevice) {
            std::cerr << "IrrlichtModelConverter: Failed to create device" << std::endl;
            return false;
        }
    }

    g_converterSmgr = g_converterDevice->getSceneManager();
    return g_converterSmgr != nullptr;
}

ConvertedModel convertModelViaIrrlicht(const std::string& filepath) {
    ConvertedModel result;
    if (!ensureDevice()) return result;

    // Set working directory to model's directory so textures can be found
    std::string modelDir;
    size_t lastSlash = filepath.find_last_of("/\\");
    if (lastSlash != std::string::npos) {
        modelDir = filepath.substr(0, lastSlash + 1);
        g_converterDevice->getFileSystem()->changeWorkingDirectoryTo(modelDir.c_str());
    }

    irr::scene::IAnimatedMesh* animMesh = g_converterSmgr->getMesh(filepath.c_str());
    if (!animMesh) return result;

    irr::scene::IMesh* mesh = animMesh->getMesh(0);
    if (!mesh) return result;

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

        // Texture name (available with EDT_BURNINGSVIDEO, null with EDT_NULL)
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

        // Extract indices (Irrlicht uses 16-bit indices)
        irr::u32 idxCount = mb->getIndexCount();
        submesh.indices.resize(idxCount);
        const irr::u16* idx16 = mb->getIndices();
        for (irr::u32 i = 0; i < idxCount; i++) {
            submesh.indices[i] = static_cast<uint32_t>(idx16[i]);
        }

        result.submeshes.push_back(std::move(submesh));
    }

    result.valid = !result.submeshes.empty();
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
