/*   Bridge Command 5.0 Ship Simulator
     Copyright (C) 2026 James Packer

     This program is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License version 2 as
     published by the Free Software Foundation */

#ifndef BC_IRRLICHT_MODEL_CONVERTER_HPP
#define BC_IRRLICHT_MODEL_CONVERTER_HPP

#include <string>
#include <vector>
#include <cstdint>

namespace bc {

struct ConvertedVertex {
    float px, py, pz;   // position
    float nx, ny, nz;   // normal
    float u, v;          // texcoord
};

struct ConvertedMaterial {
    float r = 0.8f, g = 0.8f, b = 0.8f, a = 1.0f; // diffuse color
    float er = 0, eg = 0, eb = 0;                    // emissive color
    float shininess = 0;
    std::string textureName; // may be empty (EDT_NULL can't load textures)
};

struct ConvertedSubMesh {
    std::vector<ConvertedVertex> vertices;
    std::vector<uint32_t> indices;
    ConvertedMaterial material;
};

struct ConvertedModel {
    std::vector<ConvertedSubMesh> submeshes;
    bool valid = false;
};

// Convert a model file using Irrlicht's headless device (EDT_NULL).
// Supports .x, .3ds, and other Irrlicht-supported formats.
// Uses a lazy-initialized global device with mesh caching.
// Thread-unsafe (uses global state).
ConvertedModel convertModelViaIrrlicht(const std::string& filepath);

// Release the headless Irrlicht device. Call after all conversions are done.
void shutdownIrrlichtConverter();

// Scan a binary model file (.x, .3ds) for embedded texture filenames.
// Returns list of texture names in order of appearance.
std::vector<std::string> scanTextureNames(const std::string& filepath);

} // namespace bc

#endif // BC_IRRLICHT_MODEL_CONVERTER_HPP
