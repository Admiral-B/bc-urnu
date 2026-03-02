#include "TextureUpscaler.hpp"
#include "libs/stb/stb_image.h"
#include "libs/stb/stb_image_resize2.h"
#include "libs/stb/stb_image_write.h"

#include <filesystem>
#include <functional>

namespace fs = std::filesystem;

std::string TextureUpscaler::ensureMinSize(const std::string& texPath,
                                            const std::string& cacheDir,
                                            int minSize) {
    if (texPath.empty()) return "";

    // Load image to check dimensions
    int w = 0, h = 0, channels = 0;
    if (!stbi_info(texPath.c_str(), &w, &h, &channels))
        return "";

    if (w >= minSize && h >= minSize)
        return ""; // No upscaling needed

    // Compute cache path from hash of source path
    size_t pathHash = std::hash<std::string>{}(texPath);
    std::string cachePath = cacheDir + std::to_string(pathHash) + ".png";

    // Check if cached version exists and is newer than source
    try {
        if (fs::exists(cachePath) && fs::exists(texPath)) {
            auto cacheTime = fs::last_write_time(cachePath);
            auto srcTime = fs::last_write_time(texPath);
            if (cacheTime >= srcTime)
                return cachePath;
        }
    } catch (...) {}

    // Load the actual pixel data (force RGBA)
    unsigned char* srcData = stbi_load(texPath.c_str(), &w, &h, &channels, 4);
    if (!srcData)
        return "";

    // Compute target dimensions (double until >= minSize)
    int tw = w, th = h;
    while (tw < minSize || th < minSize) {
        tw *= 2;
        th *= 2;
    }

    // Resize
    unsigned char* dstData = (unsigned char*)malloc(tw * th * 4);
    if (!dstData) {
        stbi_image_free(srcData);
        return "";
    }

    stbir_resize_uint8_linear(srcData, w, h, w * 4,
                               dstData, tw, th, tw * 4,
                               STBIR_RGBA);

    stbi_image_free(srcData);

    // Ensure cache directory exists
    try {
        fs::create_directories(cacheDir);
    } catch (...) {
        free(dstData);
        return "";
    }

    // Write as PNG
    int ok = stbi_write_png(cachePath.c_str(), tw, th, 4, dstData, tw * 4);
    free(dstData);

    return ok ? cachePath : "";
}
