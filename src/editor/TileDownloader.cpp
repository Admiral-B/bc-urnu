#include "TileDownloader.hpp"
#include "TileThreadPool.hpp"
#include "WinHTTPConnectionPool.hpp"
#include <fstream>
#include <sstream>
#include <filesystem>

namespace fs = std::filesystem;

TileDownloader::TileDownloader(const std::string& tileServerUrl,
                               const std::string& cacheDir,
                               int numWorkers)
    : serverUrl(tileServerUrl)
    , cacheDir(cacheDir)
    , userAgent("BridgeCommand/6.0 (scenario-editor)")
{
    // Ensure cache directory exists
    try {
        fs::create_directories(cacheDir);
    } catch (...) {}

    // Extract domain from server URL for rate limiting
    serverDomain = extractDomain(serverUrl);

    // Create connection pool (one HINTERNET session for lifetime)
    connPool = std::make_unique<WinHTTPConnectionPool>(userAgent);

    // Create thread pool with per-domain rate limiting
    threadPool = std::make_unique<TileThreadPool>(numWorkers, 100);
}

TileDownloader::~TileDownloader() {
    // Thread pool shutdown happens in its destructor (joins all workers)
    threadPool.reset();
    connPool.reset();
}

std::string TileDownloader::tileKey(int z, int x, int y) {
    return std::to_string(z) + "/" + std::to_string(x) + "/" + std::to_string(y);
}

std::string TileDownloader::buildUrl(int z, int x, int y) const {
    std::string url = serverUrl;
    std::string zStr = std::to_string(z);
    std::string xStr = std::to_string(x);
    std::string yStr = std::to_string(y);

    size_t pos;
    while ((pos = url.find("{z}")) != std::string::npos) url.replace(pos, 3, zStr);
    while ((pos = url.find("{x}")) != std::string::npos) url.replace(pos, 3, xStr);
    while ((pos = url.find("{y}")) != std::string::npos) url.replace(pos, 3, yStr);

    return url;
}

std::string TileDownloader::buildCachePath(int z, int x, int y) const {
    return cacheDir + "/" + std::to_string(z) + "/" + std::to_string(x) + "/" + std::to_string(y) + ".png";
}

std::string TileDownloader::extractDomain(const std::string& url) const {
    size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) return "";
    size_t hostStart = schemeEnd + 3;
    size_t pathStart = url.find('/', hostStart);
    if (pathStart == std::string::npos) return url.substr(hostStart);
    return url.substr(hostStart, pathStart - hostStart);
}

std::vector<uint8_t> TileDownloader::getTile(int z, int x, int y) {
    std::string key = tileKey(z, x, y);

    // 1. Check memory cache
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        auto it = memoryCache.find(key);
        if (it != memoryCache.end()) {
            return it->second;
        }
    }

    // 2. Check disk cache
    std::string diskPath = buildCachePath(z, x, y);
    std::vector<uint8_t> data = loadFromDisk(diskPath);
    if (!data.empty()) {
        addToMemoryCache(key, data);
        return data;
    }

    // 3. Queue for download if not already queued
    {
        std::lock_guard<std::mutex> lock(queuedMutex);
        if (queuedTiles.find(key) != queuedTiles.end()) {
            return {}; // Already queued
        }
        queuedTiles[key] = true;
    }

    // Submit download to thread pool
    std::string url = buildUrl(z, x, y);
    std::string domain = serverDomain;
    std::string cachePath = diskPath;

    threadPool->submit([this, url, key, cachePath] {
        std::vector<uint8_t> downloaded = connPool->get(url);

        if (!downloaded.empty()) {
            saveToDisk(cachePath, downloaded);
            addToMemoryCache(key, std::move(downloaded));
        }

        {
            std::lock_guard<std::mutex> lock(queuedMutex);
            queuedTiles.erase(key);
        }
    }, domain);

    return {}; // Not available yet
}

bool TileDownloader::isTileReady(int z, int x, int y) {
    std::string key = tileKey(z, x, y);

    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        if (memoryCache.find(key) != memoryCache.end()) {
            return true;
        }
    }

    std::string diskPath = buildCachePath(z, x, y);
    return fs::exists(diskPath);
}

int TileDownloader::pendingDownloads() const {
    return threadPool ? threadPool->pendingCount() : 0;
}

void TileDownloader::setUserAgent(const std::string& ua) {
    userAgent = ua;
}

void TileDownloader::addToMemoryCache(const std::string& key, std::vector<uint8_t> data) {
    std::lock_guard<std::mutex> lock(cacheMutex);
    if (memoryCache.size() >= MAX_MEMORY_CACHE) {
        if (!cacheOrder.empty()) {
            memoryCache.erase(cacheOrder.front());
            cacheOrder.erase(cacheOrder.begin());
        }
    }
    memoryCache[key] = std::move(data);
    cacheOrder.push_back(key);
}

std::vector<uint8_t> TileDownloader::loadFromDisk(const std::string& path) {
    try {
        if (!fs::exists(path)) return {};
        std::ifstream file(path, std::ios::binary);
        if (!file) return {};
        file.seekg(0, std::ios::end);
        size_t size = file.tellg();
        if (size == 0) return {};
        file.seekg(0, std::ios::beg);
        std::vector<uint8_t> data(size);
        file.read(reinterpret_cast<char*>(data.data()), size);
        return data;
    } catch (...) {
        return {};
    }
}

bool TileDownloader::saveToDisk(const std::string& path, const std::vector<uint8_t>& data) {
    try {
        fs::path p(path);
        fs::create_directories(p.parent_path());
        std::ofstream file(path, std::ios::binary);
        if (!file) return false;
        file.write(reinterpret_cast<const char*>(data.data()), data.size());
        return true;
    } catch (...) {
        return false;
    }
}
