#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace irr { namespace video { class IVideoDriver; class ITexture; } }

class TileDownloader;

// Manages tile textures for the map widget.
// Decodes PNG tile data from TileDownloader into Irrlicht textures.
class TileTextureManager {
public:
    TileTextureManager(TileDownloader* downloader, irr::video::IVideoDriver* driver);
    ~TileTextureManager();

    // Get or create a texture for a tile.
    // Returns nullptr if tile not yet available (still downloading).
    irr::video::ITexture* getTileTexture(int z, int x, int y);

    // Release textures that haven't been used recently (call once per frame)
    void evictUnused(int maxTextures = 300);

    // Release all textures
    void clear();

    // Call once per frame to advance the frame counter
    void beginFrame();

private:
    struct CachedTexture {
        irr::video::ITexture* texture = nullptr;
        int lastUsedFrame = 0;
    };

    TileDownloader* downloader;
    irr::video::IVideoDriver* driver;
    std::unordered_map<std::string, CachedTexture> textures;
    int currentFrame = 0;

    static std::string tileKey(int z, int x, int y);
    irr::video::ITexture* createTextureFromPNG(const std::string& name,
                                                const std::vector<uint8_t>& pngData);
};
