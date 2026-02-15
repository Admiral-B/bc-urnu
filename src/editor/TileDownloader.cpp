#include "TileDownloader.hpp"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#else
#ifdef WITH_CURL
#include <curl/curl.h>
#endif
#endif

namespace fs = std::filesystem;

TileDownloader::TileDownloader(const std::string& tileServerUrl, const std::string& cacheDir)
    : serverUrl(tileServerUrl)
    , cacheDir(cacheDir)
    , userAgent("BridgeCommand/6.0 (scenario-editor)")
{
    // Ensure cache directory exists
    try {
        fs::create_directories(cacheDir);
    } catch (...) {
        // Silently continue - disk cache won't work but memory cache still will
    }

    // Start background download thread
    workerThread = std::thread(&TileDownloader::downloadThreadFunc, this);
}

TileDownloader::~TileDownloader() {
    running = false;
    if (workerThread.joinable()) {
        workerThread.join();
    }
}

std::string TileDownloader::tileKey(int z, int x, int y) {
    return std::to_string(z) + "/" + std::to_string(x) + "/" + std::to_string(y);
}

std::string TileDownloader::buildUrl(int z, int x, int y) const {
    std::string url = serverUrl;
    // Replace {z}, {x}, {y} placeholders
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
        // Store in memory cache
        std::lock_guard<std::mutex> lock(cacheMutex);
        if (memoryCache.size() >= MAX_MEMORY_CACHE) {
            // Evict oldest entry
            if (!cacheOrder.empty()) {
                memoryCache.erase(cacheOrder.front());
                cacheOrder.erase(cacheOrder.begin());
            }
        }
        memoryCache[key] = data;
        cacheOrder.push_back(key);
        return data;
    }

    // 3. Queue for download if not already queued
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (queuedTiles.find(key) == queuedTiles.end()) {
            downloadQueue.push({z, x, y});
            queuedTiles[key] = true;
            pendingCount++;
        }
    }

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
    return pendingCount.load();
}

void TileDownloader::setUserAgent(const std::string& ua) {
    userAgent = ua;
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

void TileDownloader::downloadThreadFunc() {
    while (running) {
        DownloadRequest req;
        bool hasWork = false;

        {
            std::lock_guard<std::mutex> lock(queueMutex);
            if (!downloadQueue.empty()) {
                req = downloadQueue.front();
                downloadQueue.pop();
                hasWork = true;
            }
        }

        if (!hasWork) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        std::string url = buildUrl(req.z, req.x, req.y);
        std::vector<uint8_t> data = httpDownload(url);

        std::string key = tileKey(req.z, req.x, req.y);

        if (!data.empty()) {
            // Save to disk cache
            saveToDisk(buildCachePath(req.z, req.x, req.y), data);

            // Save to memory cache
            {
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
        }

        {
            std::lock_guard<std::mutex> lock(queueMutex);
            queuedTiles.erase(key);
        }
        pendingCount--;

        // Rate limit: be respectful to tile servers
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// ---- Platform-specific HTTP download ----

#ifdef _WIN32

std::vector<uint8_t> TileDownloader::httpDownload(const std::string& url) {
    std::vector<uint8_t> result;

    // Parse URL into host, path, and scheme
    // Expected format: https://host/path
    std::string host, path;
    bool useHttps = true;
    size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) return result;

    std::string scheme = url.substr(0, schemeEnd);
    if (scheme == "http") useHttps = false;

    size_t hostStart = schemeEnd + 3;
    size_t pathStart = url.find('/', hostStart);
    if (pathStart == std::string::npos) {
        host = url.substr(hostStart);
        path = "/";
    } else {
        host = url.substr(hostStart, pathStart - hostStart);
        path = url.substr(pathStart);
    }

    // Convert strings to wide chars for WinHTTP
    std::wstring wHost(host.begin(), host.end());
    std::wstring wPath(path.begin(), path.end());
    std::wstring wUA(userAgent.begin(), userAgent.end());

    HINTERNET hSession = WinHttpOpen(wUA.c_str(),
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return result;

    INTERNET_PORT port = useHttps ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    HINTERNET hConnect = WinHttpConnect(hSession, wHost.c_str(), port, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return result;
    }

    DWORD flags = useHttps ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", wPath.c_str(),
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return result;
    }

    // Set timeout to 10 seconds
    DWORD timeout = 10000;
    WinHttpSetTimeouts(hRequest, timeout, timeout, timeout, timeout);

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return result;
    }

    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return result;
    }

    // Check HTTP status code
    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize,
        WINHTTP_NO_HEADER_INDEX);

    if (statusCode != 200) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return result;
    }

    // Read response body
    DWORD bytesAvailable = 0;
    DWORD bytesRead = 0;
    do {
        if (!WinHttpQueryDataAvailable(hRequest, &bytesAvailable)) break;
        if (bytesAvailable == 0) break;

        std::vector<uint8_t> buffer(bytesAvailable);
        if (!WinHttpReadData(hRequest, buffer.data(), bytesAvailable, &bytesRead)) break;

        result.insert(result.end(), buffer.begin(), buffer.begin() + bytesRead);
    } while (bytesAvailable > 0);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return result;
}

#else // Linux/macOS

std::vector<uint8_t> TileDownloader::httpDownload(const std::string& url) {
    std::vector<uint8_t> result;

#ifdef WITH_CURL
    CURL* curl = curl_easy_init();
    if (!curl) return result;

    auto writeCallback = [](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
        auto* vec = static_cast<std::vector<uint8_t>*>(userdata);
        size_t totalBytes = size * nmemb;
        vec->insert(vec->end(), ptr, ptr + totalBytes);
        return totalBytes;
    };

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, static_cast<size_t(*)(char*, size_t, size_t, void*)>(
        [](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
            auto* vec = static_cast<std::vector<uint8_t>*>(userdata);
            size_t totalBytes = size * nmemb;
            vec->insert(vec->end(), ptr, ptr + totalBytes);
            return totalBytes;
        }));
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        result.clear();
    }

    curl_easy_cleanup(curl);
#endif

    return result;
}

#endif
