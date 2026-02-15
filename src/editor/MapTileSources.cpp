#include "MapTileSources.hpp"
#include <irrlicht.h>

// ESRI World Imagery (satellite) - note {z}/{y}/{x} order
static const char* ESRI_URL =
    "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}";

// OpenStreetMap standard tiles - {z}/{x}/{y} order
static const char* OSM_URL =
    "https://tile.openstreetmap.org/{z}/{x}/{y}.png";

// OpenSeaMap seamark overlay tiles
static const char* SEAMARK_URL =
    "https://tiles.openseamap.org/seamark/{z}/{x}/{y}.png";

MapTileSources::MapTileSources(const std::string& cacheBaseDir, irr::video::IVideoDriver* driver) {
    std::string satelliteCacheDir = cacheBaseDir + "/esri/";
    std::string streetCacheDir = cacheBaseDir + "/osm/";
    std::string seamarkCacheDir = cacheBaseDir + "/seamark/";

    satelliteDownloader = std::make_unique<TileDownloader>(ESRI_URL, satelliteCacheDir);
    satelliteDownloader->setUserAgent("BridgeCommand/6.0 (scenario-editor)");

    streetDownloader = std::make_unique<TileDownloader>(OSM_URL, streetCacheDir);
    streetDownloader->setUserAgent("BridgeCommand/6.0 (scenario-editor)");

    seamarkDownloader = std::make_unique<TileDownloader>(SEAMARK_URL, seamarkCacheDir);
    seamarkDownloader->setUserAgent("BridgeCommand/6.0 (scenario-editor)");

    satelliteTexManager = std::make_unique<TileTextureManager>(satelliteDownloader.get(), driver);
    streetTexManager = std::make_unique<TileTextureManager>(streetDownloader.get(), driver);
    seamarkTexManager = std::make_unique<TileTextureManager>(seamarkDownloader.get(), driver);

    setSource(SATELLITE);
}

MapTileSources::~MapTileSources() {
}

void MapTileSources::setSource(Source source) {
    currentSource = source;
    if (source == SATELLITE) {
        activeDownloader = satelliteDownloader.get();
        activeTexManager = satelliteTexManager.get();
    } else {
        activeDownloader = streetDownloader.get();
        activeTexManager = streetTexManager.get();
    }
}

void MapTileSources::toggleSource() {
    setSource(currentSource == SATELLITE ? STREET_MAP : SATELLITE);
}

const char* MapTileSources::getAttribution() const {
    if (currentSource == SATELLITE) {
        return "Tiles (c) Esri";
    } else {
        return "(c) OpenStreetMap contributors";
    }
}
