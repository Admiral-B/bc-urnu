#include "OSMBuildingReader.hpp"
#include "../libs/nlohmann/json.hpp"

#include <sstream>
#include <fstream>
#include <cmath>
#include <iostream>
#include <iomanip>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#else
#ifdef WITH_CURL
#include <curl/curl.h>
#endif
#endif

using json = nlohmann::json;

static const char* OVERPASS_URL = "https://overpass-api.de/api/interpreter";
static const char* USER_AGENT = "BridgeCommand/6.0 (world-generator)";

// ---- HTTP POST (platform-specific, same pattern as OpenSeaMapSource) ----

#ifdef _WIN32
std::vector<uint8_t> OSMBuildingReader::httpPost(const std::string& url,
                                                  const std::string& body,
                                                  const std::string& userAgent) {
    std::vector<uint8_t> result;

    size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) return result;

    std::string scheme = url.substr(0, schemeEnd);
    bool useHttps = (scheme != "http");

    size_t hostStart = schemeEnd + 3;
    size_t pathStart = url.find('/', hostStart);
    std::string host, path;
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
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return result;

    INTERNET_PORT port = useHttps ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    HINTERNET hConnect = WinHttpConnect(hSession, wHost.c_str(), port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return result; }

    DWORD flags = useHttps ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", wPath.c_str(),
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return result;
    }

    DWORD timeout = 120000;
    WinHttpSetTimeouts(hRequest, timeout, timeout, timeout, timeout);

    LPCWSTR contentType = L"Content-Type: application/x-www-form-urlencoded";
    if (!WinHttpSendRequest(hRequest, contentType, (DWORD)-1,
            (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0) ||
        !WinHttpReceiveResponse(hRequest, NULL)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return result;
    }

    DWORD statusCode = 0, statusCodeSize = sizeof(statusCode);
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

    DWORD bytesAvailable = 0, bytesRead = 0;
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

std::vector<uint8_t> OSMBuildingReader::httpPost(const std::string& url,
                                                  const std::string& body,
                                                  const std::string& userAgent) {
    std::vector<uint8_t> result;
#ifdef WITH_CURL
    CURL* curl = curl_easy_init();
    if (!curl) return result;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
        +[](char* ptr, size_t size, size_t nmemb, void* ud) -> size_t {
            auto* vec = static_cast<std::vector<uint8_t>*>(ud);
            size_t total = size * nmemb;
            vec->insert(vec->end(), ptr, ptr + total);
            return total;
        });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) result.clear();
    curl_easy_cleanup(curl);
#endif
    return result;
}
#endif

// ---- Height estimation from OSM tags ----

float OSMBuildingReader::estimateHeight(const std::string& heightStr,
                                         const std::string& levelsStr,
                                         const std::string& type) {
    // Explicit height tag takes priority
    if (!heightStr.empty()) {
        try { return std::stof(heightStr); }
        catch (...) {}
    }

    // Estimate from number of levels (3m per storey)
    if (!levelsStr.empty()) {
        try { return std::stof(levelsStr) * 3.0f; }
        catch (...) {}
    }

    // Default heights by type
    if (type == "church" || type == "cathedral") return 15.0f;
    if (type == "industrial" || type == "warehouse") return 8.0f;
    if (type == "garage" || type == "shed") return 3.0f;
    if (type == "commercial" || type == "office") return 12.0f;

    // Harbour structures (taller than real-world for visibility from bridge)
    if (type == "dam") return 8.0f;
    if (type == "breakwater") return 5.0f;
    if (type == "pier") return 3.0f;
    if (type == "jetty") return 2.0f;
    if (type == "groyne") return 1.5f;
    if (type == "quay") return 3.0f;
    if (type == "wharf") return 3.0f;
    if (type == "slipway") return 1.0f;
    if (type == "boat_ramp") return 1.5f;
    if (type == "dockyard") return 6.0f;
    if (type == "dry_dock") return 4.0f;
    if (type == "landing_stage") return 2.0f;
    if (type == "crane") return 25.0f;
    if (type == "storage_tank") return 12.0f;
    if (type == "silo") return 15.0f;
    if (type == "water_tower") return 20.0f;
    if (type == "power_tower") return 30.0f;
    if (type == "bridge") return 6.0f;
    if (type == "embankment") return 3.0f;

    return 9.0f; // ~3 storeys
}

// ---- Classify OSM building=* tag to simple categories ----

std::string OSMBuildingReader::classifyType(const std::string& osmType) {
    if (osmType == "church" || osmType == "cathedral" || osmType == "chapel")
        return "church";
    if (osmType == "industrial" || osmType == "warehouse" || osmType == "factory")
        return "industrial";
    if (osmType == "commercial" || osmType == "retail" || osmType == "office" ||
        osmType == "supermarket" || osmType == "shop")
        return "commercial";
    if (osmType == "garage" || osmType == "garages" || osmType == "shed" ||
        osmType == "hut" || osmType == "barn")
        return "garage";
    if (osmType == "apartments" || osmType == "dormitory")
        return "apartments";
    // residential, house, detached, semi, terrace, yes, etc.
    return "residential";
}

// ---- Overpass API query ----

bool OSMBuildingReader::query(double minLat, double maxLat,
                               double minLon, double maxLon,
                               ProgressCallback progress) {
    buildings.clear();
    barrierLines.clear();
    islandPolygons.clear();
    queryDone = false;
    errorMsg.clear();

    if (progress) progress("Building Overpass query for buildings and structures...");

    // Query building ways AND harbour structures with full geometry in bbox
    std::ostringstream ql;
    ql << std::fixed;
    ql.precision(6);
    ql << "[out:json][timeout:90];"
       << "(way[\"building\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon
       << ");"
       << "way[\"man_made\"~\"pier|jetty|groyne|quay|wharf\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon
       << ");"
       << "way[\"man_made\"=\"breakwater\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon
       << ");"
       << "way[\"waterway\"=\"dam\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon
       << ");"
       << "way[\"waterway\"=\"weir\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon
       << ");"
       << "way[\"man_made\"=\"slipway\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon
       << ");"
       << "way[\"leisure\"=\"slipway\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon
       << ");"
       << "way[\"place\"~\"island|islet\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon
       << ");"
       << "way[\"natural\"~\"rock|bare_rock\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon
       << ");"
       << "way[\"man_made\"=\"boat_ramp\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon
       << ");"
       << "way[\"man_made\"=\"dockyard\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon
       << ");"
       << "way[\"man_made\"=\"dry_dock\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon
       << ");"
       << "way[\"man_made\"=\"landing_stage\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon
       << ");"
       << "way[\"man_made\"=\"crane\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon
       << ");"
       << "way[\"man_made\"=\"embankment\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon
       << ");"
       << "way[\"man_made\"=\"bridge\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon
       << ");"
       << "way[\"man_made\"=\"storage_tank\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon
       << ");"
       << "way[\"man_made\"=\"silo\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon
       << ");"
       << "way[\"man_made\"=\"water_tower\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon
       << ");"
       << "way[\"man_made\"=\"power_tower\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon
       << ");"
       << "way[\"waterway\"=\"dry_dock\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon
       << ");"
       << "way[\"waterway\"=\"boatyard\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon
       << "););"
       << "out geom;";

    std::string postBody = "data=" + ql.str();

    const char* overpassServers[] = {
        OVERPASS_URL,
        "https://overpass.kumi.systems/api/interpreter"
    };

    std::vector<uint8_t> response;
    for (int attempt = 0; attempt < 2 && response.empty(); attempt++) {
        if (attempt > 0 && progress) progress("Retrying with fallback Overpass server...");
        else if (progress) progress("Querying Overpass API for buildings and structures...");
        response = httpPost(overpassServers[attempt], postBody, USER_AGENT);
    }
    if (response.empty()) {
        errorMsg = "Overpass API returned empty response";
        return false;
    }

    if (progress) progress("Parsing building and structure footprints...");

    std::string jsonStr(response.begin(), response.end());
    if (!parseResponse(jsonStr)) {
        return false;
    }

    queryDone = true;
    if (progress) {
        int structCount = 0;
        for (const auto& b : buildings) {
            if (b.isStructure) structCount++;
        }
        std::ostringstream msg;
        msg << "Found " << buildings.size() << " buildings/structures";
        if (structCount > 0) msg << " (" << structCount << " harbour structures)";
        progress(msg.str());
    }

    return true;
}

// ---- JSON parsing ----

bool OSMBuildingReader::parseResponse(const std::string& jsonStr) {
    json root;
    try {
        root = json::parse(jsonStr);
    } catch (const json::parse_error& e) {
        errorMsg = std::string("JSON parse error: ") + e.what();
        return false;
    }

    if (!root.contains("elements") || !root["elements"].is_array()) {
        errorMsg = "No 'elements' array in Overpass response";
        return false;
    }

    for (const auto& elem : root["elements"]) {
        if (!elem.contains("type") || elem["type"] != "way")
            continue;
        if (!elem.contains("geometry") || !elem["geometry"].is_array())
            continue;

        const auto& geom = elem["geometry"];
        if (geom.size() < 3) continue; // Need at least a triangle

        BuildingFootprint fp;

        // Extract polygon outline
        for (const auto& pt : geom) {
            if (pt.contains("lat") && pt.contains("lon")) {
                fp.outline.emplace_back(
                    pt["lat"].get<double>(),
                    pt["lon"].get<double>()
                );
            }
        }

        if (fp.outline.size() < 2) continue;

        // Extract tags
        std::string heightStr, levelsStr, buildingType = "yes";
        std::string name;
        bool isStructure = false;
        std::string structureType;

        if (elem.contains("tags") && elem["tags"].is_object()) {
            const auto& tags = elem["tags"];

            // Island/islet/rock polygons: store outline and skip building processing
            if (fp.outline.size() >= 3) {
                bool isIsland = false;
                if (tags.contains("place")) {
                    std::string place = tags["place"].get<std::string>();
                    isIsland = (place == "island" || place == "islet");
                }
                if (!isIsland && tags.contains("natural")) {
                    std::string nat = tags["natural"].get<std::string>();
                    isIsland = (nat == "rock" || nat == "bare_rock");
                }
                if (isIsland) {
                    islandPolygons.push_back(fp.outline);
                    continue; // not a building
                }
            }

            if (tags.contains("building"))
                buildingType = tags["building"].get<std::string>();

            // Harbour structures: man_made=breakwater|pier|jetty|groyne|quay|wharf|slipway, waterway=dam
            if (tags.contains("man_made")) {
                structureType = tags["man_made"].get<std::string>();
                if (structureType == "pier" ||
                    structureType == "jetty" || structureType == "groyne" ||
                    structureType == "breakwater" ||
                    structureType == "quay" || structureType == "wharf" ||
                    structureType == "slipway" ||
                    structureType == "boat_ramp" ||
                    structureType == "dockyard" ||
                    structureType == "dry_dock" ||
                    structureType == "landing_stage" ||
                    structureType == "crane" ||
                    structureType == "embankment" ||
                    structureType == "bridge" ||
                    structureType == "storage_tank" ||
                    structureType == "silo" ||
                    structureType == "water_tower" ||
                    structureType == "power_tower") {
                    isStructure = true;
                }
            }
            if (tags.contains("leisure")) {
                std::string leisure = tags["leisure"].get<std::string>();
                if (leisure == "slipway") {
                    isStructure = true;
                    structureType = "slipway";
                }
            }
            if (tags.contains("waterway")) {
                std::string ww = tags["waterway"].get<std::string>();
                if (ww == "dam" || ww == "weir") {
                    isStructure = true;
                    structureType = "dam";
                } else if (ww == "dry_dock") {
                    isStructure = true;
                    structureType = "dry_dock";
                } else if (ww == "boatyard") {
                    isStructure = true;
                    structureType = "dockyard";
                }
            }

            if (tags.contains("building:height"))
                heightStr = tags["building:height"].get<std::string>();
            else if (tags.contains("height"))
                heightStr = tags["height"].get<std::string>();

            if (tags.contains("building:levels"))
                levelsStr = tags["building:levels"].get<std::string>();

            if (tags.contains("name"))
                name = tags["name"].get<std::string>();
        }

        if (isStructure) {
            fp.type = structureType;
            fp.isStructure = true;
            fp.height = estimateHeight(heightStr, levelsStr, structureType);
        } else {
            fp.type = classifyType(buildingType);
            fp.height = estimateHeight(heightStr, levelsStr, fp.type);
        }
        fp.name = name;

        // Check if the way is a closed polygon (first == last point)
        bool isClosed = (fp.outline.size() >= 3 &&
                         std::abs(fp.outline.front().first - fp.outline.back().first) < 1e-7 &&
                         std::abs(fp.outline.front().second - fp.outline.back().second) < 1e-7);

        // Save original centerline for dams/breakwaters before polygon buffering
        if (isStructure && (structureType == "dam" || structureType == "breakwater") &&
            fp.outline.size() >= 2) {
            barrierLines.push_back(fp.outline);
        }

        if (!isClosed && fp.isStructure) {
            if (fp.outline.size() >= 2) {
                // Buffer open-way structures (piers/jetties/groynes) into thin polygons
                double widthM = (structureType == "dam" || structureType == "breakwater") ? 6.0 : 3.0;
                double widthDeg = widthM / 111320.0;

                std::vector<std::pair<double,double>> left, right;
                for (size_t i = 0; i < fp.outline.size(); i++) {
                    double dx = 0, dy = 0;
                    if (i == 0) {
                        dy = fp.outline[1].first - fp.outline[0].first;
                        dx = fp.outline[1].second - fp.outline[0].second;
                    } else if (i == fp.outline.size() - 1) {
                        dy = fp.outline[i].first - fp.outline[i-1].first;
                        dx = fp.outline[i].second - fp.outline[i-1].second;
                    } else {
                        dy = fp.outline[i+1].first - fp.outline[i-1].first;
                        dx = fp.outline[i+1].second - fp.outline[i-1].second;
                    }
                    double len = std::sqrt(dx*dx + dy*dy);
                    if (len < 1e-12) len = 1e-12;
                    double px = -dy / len * widthDeg * 0.5;
                    double py =  dx / len * widthDeg * 0.5;

                    left.emplace_back(fp.outline[i].first + py, fp.outline[i].second + px);
                    right.emplace_back(fp.outline[i].first - py, fp.outline[i].second - px);
                }
                fp.outline.clear();
                fp.outline.insert(fp.outline.end(), left.begin(), left.end());
                fp.outline.insert(fp.outline.end(), right.rbegin(), right.rend());
                fp.outline.push_back(fp.outline.front());
            }
        }

        if (!isClosed && !fp.isStructure && fp.outline.size() < 3) continue;

        buildings.push_back(std::move(fp));
    }

    return true;
}

// ---- Disk cache (simple text format) ----

bool OSMBuildingReader::saveCache(const std::string& path) const {
    std::ofstream f(path);
    if (!f.is_open()) return false;

    f << std::fixed << std::setprecision(8);
    f << buildings.size() << "\n";
    for (const auto& b : buildings) {
        f << b.outline.size() << " " << b.height << " " << (b.isStructure ? 1 : 0) << " " << b.type << " " << b.name << "\n";
        for (const auto& [lat, lon] : b.outline) {
            f << lat << " " << lon << "\n";
        }
    }

    // Barrier lines section (appended after buildings for backwards compatibility)
    f << "BARRIERS " << barrierLines.size() << "\n";
    for (const auto& line : barrierLines) {
        f << line.size() << "\n";
        for (const auto& [lat, lon] : line) {
            f << lat << " " << lon << "\n";
        }
    }

    // Island polygons section
    f << "ISLANDS " << islandPolygons.size() << "\n";
    for (const auto& poly : islandPolygons) {
        f << poly.size() << "\n";
        for (const auto& [lat, lon] : poly) {
            f << lat << " " << lon << "\n";
        }
    }
    return true;
}

bool OSMBuildingReader::loadCache(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return false;

    buildings.clear();
    barrierLines.clear();
    islandPolygons.clear();
    queryDone = false;

    size_t count = 0;
    if (!(f >> count)) return false;

    buildings.reserve(count);
    for (size_t i = 0; i < count; i++) {
        BuildingFootprint fp;
        size_t npts = 0;
        int isStruct = 0;
        if (!(f >> npts >> fp.height >> isStruct)) return false;
        fp.isStructure = (isStruct != 0);

        // Read type (single word)
        if (!(f >> fp.type)) return false;

        // Read rest of line as name (may contain spaces or be empty)
        std::getline(f, fp.name);
        // Trim leading space
        if (!fp.name.empty() && fp.name[0] == ' ')
            fp.name = fp.name.substr(1);

        fp.outline.resize(npts);
        for (size_t j = 0; j < npts; j++) {
            if (!(f >> fp.outline[j].first >> fp.outline[j].second))
                return false;
        }
        buildings.push_back(std::move(fp));
    }

    // Try to read extended sections (may not exist in older cache files)
    std::string marker;
    size_t sectionCount = 0;
    while (f >> marker >> sectionCount) {
        if (marker == "BARRIERS") {
            barrierLines.reserve(sectionCount);
            for (size_t i = 0; i < sectionCount; i++) {
                size_t npts = 0;
                if (!(f >> npts)) break;
                std::vector<std::pair<double, double>> line(npts);
                for (size_t j = 0; j < npts; j++) {
                    if (!(f >> line[j].first >> line[j].second)) break;
                }
                if (line.size() == npts) {
                    barrierLines.push_back(std::move(line));
                }
            }
        } else if (marker == "ISLANDS") {
            islandPolygons.reserve(sectionCount);
            for (size_t i = 0; i < sectionCount; i++) {
                size_t npts = 0;
                if (!(f >> npts)) break;
                std::vector<std::pair<double, double>> poly(npts);
                for (size_t j = 0; j < npts; j++) {
                    if (!(f >> poly[j].first >> poly[j].second)) break;
                }
                if (poly.size() == npts) {
                    islandPolygons.push_back(std::move(poly));
                }
            }
        } else {
            break; // unknown section
        }
    }

    queryDone = true;
    return true;
}
