#pragma once

#include "TileDownloader.hpp"
#include "TileTextureManager.hpp"
#include <memory>
#include <string>

namespace irr { namespace video { class IVideoDriver; } }

// Manages tile sources (satellite + street map + seamark overlay).
class MapTileSources {
public:
    enum Source { SATELLITE, STREET_MAP };

    MapTileSources(const std::string& cacheBaseDir, irr::video::IVideoDriver* driver);
    ~MapTileSources();

    // Get the active downloader and texture manager
    TileDownloader* getDownloader() { return activeDownloader; }
    TileTextureManager* getTextureManager() { return activeTexManager; }

    // Toggle between satellite and street map
    void setSource(Source source);
    Source getSource() const { return currentSource; }
    void toggleSource();

    // Seamark overlay (independent of base source)
    TileDownloader* getSeamarkDownloader() { return seamarkDownloader.get(); }
    TileTextureManager* getSeamarkTextureManager() { return seamarkTexManager.get(); }
    bool isSeamarkEnabled() const { return seamarkEnabled; }
    void setSeamarkEnabled(bool enabled) { seamarkEnabled = enabled; }
    void toggleSeamark() { seamarkEnabled = !seamarkEnabled; }

    // Attribution text for the current source
    const char* getAttribution() const;

private:
    Source currentSource = SATELLITE;
    TileDownloader* activeDownloader = nullptr;
    TileTextureManager* activeTexManager = nullptr;

    std::unique_ptr<TileDownloader> satelliteDownloader;
    std::unique_ptr<TileDownloader> streetDownloader;
    std::unique_ptr<TileTextureManager> satelliteTexManager;
    std::unique_ptr<TileTextureManager> streetTexManager;

    // OpenSeaMap seamark overlay
    bool seamarkEnabled = false;
    std::unique_ptr<TileDownloader> seamarkDownloader;
    std::unique_ptr<TileTextureManager> seamarkTexManager;
};
