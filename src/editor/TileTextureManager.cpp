#define STB_IMAGE_IMPLEMENTATION
#include "../libs/stb/stb_image.h"

#include "TileTextureManager.hpp"
#include "TileDownloader.hpp"
#include <irrlicht.h>
#include <algorithm>

TileTextureManager::TileTextureManager(TileDownloader* downloader, irr::video::IVideoDriver* driver)
    : downloader(downloader)
    , driver(driver)
{
}

TileTextureManager::~TileTextureManager() {
    clear();
}

std::string TileTextureManager::tileKey(int z, int x, int y) {
    return std::to_string(z) + "/" + std::to_string(x) + "/" + std::to_string(y);
}

irr::video::ITexture* TileTextureManager::getTileTexture(int z, int x, int y) {
    std::string key = tileKey(z, x, y);

    // Check texture cache
    auto it = textures.find(key);
    if (it != textures.end()) {
        it->second.lastUsedFrame = currentFrame;
        return it->second.texture;
    }

    // Try to get tile data from downloader
    std::vector<uint8_t> pngData = downloader->getTile(z, x, y);
    if (pngData.empty()) {
        return nullptr; // Not available yet
    }

    // Decode PNG and create texture
    irr::video::ITexture* tex = createTextureFromPNG(key, pngData);
    if (tex) {
        textures[key] = {tex, currentFrame};
    }
    return tex;
}

void TileTextureManager::evictUnused(int maxTextures) {
    if (static_cast<int>(textures.size()) <= maxTextures) return;

    // Find and remove least recently used textures
    std::vector<std::pair<std::string, int>> entries;
    entries.reserve(textures.size());
    for (auto& kv : textures) {
        entries.push_back({kv.first, kv.second.lastUsedFrame});
    }

    // Sort by last used frame (oldest first)
    std::sort(entries.begin(), entries.end(),
        [](const auto& a, const auto& b) { return a.second < b.second; });

    // Evict oldest until we're at the limit
    int toRemove = static_cast<int>(textures.size()) - maxTextures;
    for (int i = 0; i < toRemove && i < static_cast<int>(entries.size()); i++) {
        auto it = textures.find(entries[i].first);
        if (it != textures.end()) {
            if (it->second.texture) {
                driver->removeTexture(it->second.texture);
            }
            textures.erase(it);
        }
    }
}

void TileTextureManager::clear() {
    for (auto& kv : textures) {
        if (kv.second.texture) {
            driver->removeTexture(kv.second.texture);
        }
    }
    textures.clear();
}

void TileTextureManager::beginFrame() {
    currentFrame++;
}

irr::video::ITexture* TileTextureManager::createTextureFromPNG(
    const std::string& name, const std::vector<uint8_t>& pngData)
{
    int width, height, channels;
    unsigned char* pixels = stbi_load_from_memory(
        pngData.data(), static_cast<int>(pngData.size()),
        &width, &height, &channels, 4); // Force RGBA

    if (!pixels) return nullptr;

    // Create an Irrlicht image and copy pixel data into it
    irr::core::dimension2d<irr::u32> size(width, height);
    irr::video::IImage* image = driver->createImage(
        irr::video::ECF_A8R8G8B8, size);

    if (!image) {
        stbi_image_free(pixels);
        return nullptr;
    }

    // Copy pixel data (stb gives RGBA, Irrlicht wants BGRA/ARGB)
    irr::u32* dest = (irr::u32*)image->getData();
    for (int i = 0; i < width * height; i++) {
        unsigned char r = pixels[i * 4 + 0];
        unsigned char g = pixels[i * 4 + 1];
        unsigned char b = pixels[i * 4 + 2];
        unsigned char a = pixels[i * 4 + 3];
        // Irrlicht A8R8G8B8 format: AAAA RRRR GGGG BBBB
        dest[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }

    stbi_image_free(pixels);

    // Create texture from image
    irr::io::path texName(name.c_str());
    irr::video::ITexture* tex = driver->addTexture(texName, image);
    image->drop();

    return tex;
}
