#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <functional>
#include <memory>

class TileThreadPool;
class WinHTTPConnectionPool;

class TileDownloader {
public:
    // tileServerUrl: URL template with {z}, {x}, {y} placeholders
    //   e.g. "https://tile.openstreetmap.org/{z}/{x}/{y}.png"
    // cacheDir: local directory for cached tiles
    //   e.g. "%APPDATA%/Bridge Command/tilecache/osm/"
    // numWorkers: number of download threads (default 4)
    TileDownloader(const std::string& tileServerUrl, const std::string& cacheDir,
                   int numWorkers = 4);
    ~TileDownloader();

    // Request a tile. Returns immediately.
    // If tile is in memory or disk cache, returns PNG bytes.
    // If not cached, queues HTTP download (async).
    // Returns empty vector if not yet available.
    std::vector<uint8_t> getTile(int z, int x, int y);

    // Check if a tile is available (cached or downloaded)
    bool isTileReady(int z, int x, int y);

    // How many downloads are pending?
    int pendingDownloads() const;

    // Set User-Agent header (required by OSM tile usage policy)
    void setUserAgent(const std::string& ua);

    // Build URL from template (public for testing)
    std::string buildUrl(int z, int x, int y) const;

private:
    std::string buildCachePath(int z, int x, int y) const;
    static std::string tileKey(int z, int x, int y);
    std::string extractDomain(const std::string& url) const;
    std::vector<uint8_t> loadFromDisk(const std::string& path);
    bool saveToDisk(const std::string& path, const std::vector<uint8_t>& data);

    void addToMemoryCache(const std::string& key, std::vector<uint8_t> data);

    std::string serverUrl;
    std::string cacheDir;
    std::string userAgent;
    std::string serverDomain; // cached domain extracted from serverUrl

    // Thread pool + connection pool (owned)
    std::unique_ptr<TileThreadPool> threadPool;
    std::unique_ptr<WinHTTPConnectionPool> connPool;

    // In-memory LRU cache (max ~200 tiles)
    std::mutex cacheMutex;
    std::unordered_map<std::string, std::vector<uint8_t>> memoryCache;
    std::vector<std::string> cacheOrder; // For LRU eviction
    static const size_t MAX_MEMORY_CACHE = 200;

    // Track tiles already queued to avoid duplicates
    std::mutex queuedMutex;
    std::unordered_map<std::string, bool> queuedTiles;
};
