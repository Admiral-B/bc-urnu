#include "ENCCatalog.hpp"
#include "../libs/nlohmann/json.hpp"

#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <chrono>
#include <iostream>

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
using json = nlohmann::json;

static const char* NOAA_CATALOG_URL = "https://www.charts.noaa.gov/InteractiveCatalog/data/enc.json";
static const char* NOAA_CHART_BASE_URL = "https://www.charts.noaa.gov/ENCs/";
static const char* USER_AGENT = "BridgeCommand/6.0 (scenario-editor)";

// ---- HTTP Download (platform-specific) ----

#ifdef _WIN32
std::vector<uint8_t> ENCCatalog::httpDownload(const std::string& url,
                                                const std::string& userAgent) {
    std::vector<uint8_t> result;

    // Parse URL
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

    // 30 second timeout for catalog download (larger file)
    DWORD timeout = 30000;
    WinHttpSetTimeouts(hRequest, timeout, timeout, timeout, timeout);

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(hRequest, NULL)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return result;
    }

    // Check status
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

    // Read response
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

std::vector<uint8_t> ENCCatalog::httpDownload(const std::string& url,
                                                const std::string& userAgent) {
    std::vector<uint8_t> result;
#ifdef WITH_CURL
    CURL* curl = curl_easy_init();
    if (!curl) return result;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
        +[](char* ptr, size_t size, size_t nmemb, void* ud) -> size_t {
            auto* vec = static_cast<std::vector<uint8_t>*>(ud);
            size_t total = size * nmemb;
            vec->insert(vec->end(), ptr, ptr + total);
            return total;
        });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) result.clear();
    curl_easy_cleanup(curl);
#endif
    return result;
}
#endif

// ---- Catalog loading ----

bool ENCCatalog::loadCatalog(const std::string& cacheDir) {
    catalog.clear();

    try {
        fs::create_directories(cacheDir);
    } catch (...) {}

    catalogCachePath = cacheDir + "/enc.json";

    // Check if cached catalog is fresh (less than 24 hours old)
    bool useCached = false;
    try {
        if (fs::exists(catalogCachePath)) {
            auto modTime = fs::last_write_time(catalogCachePath);
            auto now = fs::file_time_type::clock::now();
            auto age = std::chrono::duration_cast<std::chrono::hours>(now - modTime);
            useCached = (age.count() < 24);
        }
    } catch (...) {}

    std::string jsonStr;

    if (useCached) {
        // Load from cache
        std::ifstream file(catalogCachePath);
        if (file) {
            std::ostringstream ss;
            ss << file.rdbuf();
            jsonStr = ss.str();
        }
    }

    if (jsonStr.empty()) {
        // Download fresh catalog
        std::vector<uint8_t> data = httpDownload(NOAA_CATALOG_URL, USER_AGENT);
        if (data.empty()) {
            // Try loading stale cache as fallback
            std::ifstream file(catalogCachePath);
            if (file) {
                std::ostringstream ss;
                ss << file.rdbuf();
                jsonStr = ss.str();
            }
            if (jsonStr.empty()) return false;
        } else {
            jsonStr = std::string(data.begin(), data.end());

            // Save to cache
            try {
                std::ofstream cacheFile(catalogCachePath, std::ios::binary);
                if (cacheFile) {
                    cacheFile.write(jsonStr.data(), jsonStr.size());
                }
            } catch (...) {}
        }
    }

    return parseCatalogJSON(jsonStr);
}

bool ENCCatalog::parseCatalogJSON(const std::string& jsonStr) {
    try {
        json root = json::parse(jsonStr);

        const json& encs = root.contains("encs") ? root["encs"] : root;
        if (!encs.is_array()) return false;

        for (const auto& entry : encs) {
            ChartInfo info;

            if (entry.contains("cnum") && entry["cnum"].is_string())
                info.id = entry["cnum"].get<std::string>();
            else
                continue; // Skip entries without chart number

            if (entry.contains("title") && entry["title"].is_string())
                info.title = entry["title"].get<std::string>();

            if (entry.contains("scale") && entry["scale"].is_string())
                info.scale = parseScale(entry["scale"].get<std::string>());

            if (entry.contains("mbr") && entry["mbr"].is_array() && entry["mbr"].size() >= 4) {
                info.minLat = entry["mbr"][0].get<double>();
                info.minLon = entry["mbr"][1].get<double>();
                info.maxLat = entry["mbr"][2].get<double>();
                info.maxLon = entry["mbr"][3].get<double>();
            } else {
                continue; // Skip entries without bounding box
            }

            info.downloadUrl = std::string(NOAA_CHART_BASE_URL) + info.id + ".zip";

            catalog.push_back(std::move(info));
        }

        return !catalog.empty();
    } catch (const std::exception& e) {
        std::cerr << "ENCCatalog: JSON parse error: " << e.what() << std::endl;
        return false;
    }
}

int ENCCatalog::parseScale(const std::string& scaleStr) {
    // Parse "1:22,000" -> 22000
    size_t colonPos = scaleStr.find(':');
    if (colonPos == std::string::npos) return 0;

    std::string numStr;
    for (size_t i = colonPos + 1; i < scaleStr.size(); i++) {
        if (scaleStr[i] >= '0' && scaleStr[i] <= '9') {
            numStr += scaleStr[i];
        }
    }

    try {
        return std::stoi(numStr);
    } catch (...) {
        return 0;
    }
}

// ---- Chart queries ----

std::vector<ENCCatalog::ChartInfo> ENCCatalog::findChartsForArea(
    double minLat, double maxLat, double minLon, double maxLon) const
{
    std::vector<ChartInfo> results;

    for (const auto& chart : catalog) {
        // Bounding box intersection test
        if (chart.minLat <= maxLat && chart.maxLat >= minLat &&
            chart.minLon <= maxLon && chart.maxLon >= minLon) {
            results.push_back(chart);
        }
    }

    // Sort by scale (most detailed first = lowest scale number)
    std::sort(results.begin(), results.end(),
        [](const ChartInfo& a, const ChartInfo& b) {
            return a.scale < b.scale;
        });

    return results;
}

// ---- Chart download ----

bool ENCCatalog::isChartCached(const ChartInfo& chart,
                                const std::string& chartDir) const {
    // Look for .000 file in chart directory
    std::string chartPath = chartDir + "/" + chart.id;
    try {
        if (!fs::exists(chartPath)) return false;
        for (const auto& entry : fs::directory_iterator(chartPath)) {
            if (entry.path().extension() == ".000") return true;
        }
    } catch (...) {}
    return false;
}

std::string ENCCatalog::downloadChart(const ChartInfo& chart,
                                       const std::string& chartDir) {
    std::string outputDir = chartDir + "/" + chart.id;

    // Check if already extracted
    try {
        if (fs::exists(outputDir)) {
            for (const auto& entry : fs::directory_iterator(outputDir)) {
                if (entry.path().extension() == ".000") {
                    return entry.path().string();
                }
            }
        }
    } catch (...) {}

    // Download ZIP
    std::vector<uint8_t> zipData = httpDownload(chart.downloadUrl, USER_AGENT);
    if (zipData.empty()) {
        std::cerr << "ENCCatalog: Failed to download " << chart.downloadUrl << std::endl;
        return "";
    }

    // Save ZIP to temp file
    try {
        fs::create_directories(outputDir);
    } catch (...) {
        return "";
    }

    std::string zipPath = outputDir + "/" + chart.id + ".zip";
    {
        std::ofstream zipFile(zipPath, std::ios::binary);
        if (!zipFile) return "";
        zipFile.write(reinterpret_cast<const char*>(zipData.data()), zipData.size());
    }

    // Extract
    std::string result = extractChartZip(zipPath, outputDir);

    // Clean up ZIP
    try {
        fs::remove(zipPath);
    } catch (...) {}

    return result;
}

std::string ENCCatalog::extractChartZip(const std::string& zipPath,
                                         const std::string& outputDir) {
#ifdef _WIN32
    // Use PowerShell Expand-Archive
    std::string cmd = "powershell -NoProfile -Command \"Expand-Archive -Force -Path '";
    cmd += zipPath;
    cmd += "' -DestinationPath '";
    cmd += outputDir;
    cmd += "'\" 2>NUL";
    int ret = system(cmd.c_str());
    if (ret != 0) {
        std::cerr << "ENCCatalog: ZIP extraction failed for " << zipPath << std::endl;
        return "";
    }
#else
    std::string cmd = "unzip -o -q \"" + zipPath + "\" -d \"" + outputDir + "\" 2>/dev/null";
    int ret = system(cmd.c_str());
    if (ret != 0) {
        std::cerr << "ENCCatalog: ZIP extraction failed for " << zipPath << std::endl;
        return "";
    }
#endif

    // Find the .000 file (may be in a subdirectory)
    try {
        for (const auto& entry : fs::recursive_directory_iterator(outputDir)) {
            if (entry.path().extension() == ".000") {
                return entry.path().string();
            }
        }
    } catch (...) {}

    std::cerr << "ENCCatalog: No .000 file found in " << zipPath << std::endl;
    return "";
}
