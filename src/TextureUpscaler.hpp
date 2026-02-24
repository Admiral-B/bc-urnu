#pragma once

#include <string>

namespace TextureUpscaler {
    // If texture at texPath has either dimension < minSize, create a bilinear-upscaled
    // version in cacheDir and return its path. Returns empty string if no upscaling needed
    // or on error. Cached files are reused if newer than source.
    std::string ensureMinSize(const std::string& texPath,
                              const std::string& cacheDir,
                              int minSize = 512);
}
