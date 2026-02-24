#include "OpenSeaMapSource.hpp"
#include "../libs/nlohmann/json.hpp"

#include <sstream>
#include <cmath>
#include <algorithm>
#include <iostream>

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

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using json = nlohmann::json;

static const char* OVERPASS_URL = "https://overpass-api.de/api/interpreter";
static const char* USER_AGENT = "BridgeCommand/6.0 (scenario-editor)";

// ---- HTTP POST (platform-specific) ----

#ifdef _WIN32
std::vector<uint8_t> OpenSeaMapSource::httpPost(const std::string& url,
                                                 const std::string& body,
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
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", wPath.c_str(),
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return result;
    }

    // 120 second timeout (Overpass can be slow for large areas)
    DWORD timeout = 120000;
    WinHttpSetTimeouts(hRequest, timeout, timeout, timeout, timeout);

    // Send POST with form-encoded body
    LPCWSTR contentType = L"Content-Type: application/x-www-form-urlencoded";
    if (!WinHttpSendRequest(hRequest, contentType, (DWORD)-1,
            (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0) ||
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

std::vector<uint8_t> OpenSeaMapSource::httpPost(const std::string& url,
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

// ---- Overpass API query ----

bool OpenSeaMapSource::query(double minLat, double maxLat,
                              double minLon, double maxLon,
                              ProgressCallback progress) {
    buoys.clear();
    lights.clear();
    landmarks.clear();
    queryDone = false;
    errorMsg.clear();

    if (progress) progress("Building Overpass query...");

    // Overpass QL: fetch all seamark nodes in bounding box
    std::ostringstream ql;
    ql << std::fixed;
    ql.precision(6);
    ql << "[out:json][timeout:60];"
       << "(node[\"seamark:type\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon
       << ");"
       << "node[\"man_made\"~\"tower|chimney|lighthouse|mast\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon
       << ");"
       << "way[\"man_made\"~\"tower|chimney|lighthouse|mast\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon
       << ");"
       << "way[\"seamark:type\"~\"light_major|light_minor\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon
       << ");"
       << "node[\"building\"~\"church|cathedral|chapel\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon
       << ");"
       << "way[\"building\"~\"church|cathedral|chapel\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon
       << ");"
       << "node[\"seamark:type\"=\"wreck\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon
       << ");"
       << "node[\"seamark:type\"=\"rock\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon
       << ");"
       << "node[\"seamark:type\"=\"obstruction\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon
       << ");"
       << "node[\"seamark:type\"=\"mooring\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon
       << ");"
       << "node[\"seamark:type\"=\"anchorage\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon
       << ");"
       << "node[\"seamark:type\"=\"harbour\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon
       << ");"
       << "node[\"seamark:type\"=\"offshore_platform\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon
       << ");"
       << "way[\"seamark:type\"=\"restricted_area\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon
       << ");"
       << "way[\"seamark:type\"=\"anchorage\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon
       << "););"
       << "out body center;";

    std::string postBody = "data=" + ql.str();

    const char* overpassServers[] = {
        OVERPASS_URL,
        "https://overpass.kumi.systems/api/interpreter"
    };

    std::vector<uint8_t> response;
    for (int attempt = 0; attempt < 2 && response.empty(); attempt++) {
        if (attempt > 0 && progress) progress("Retrying with fallback Overpass server...");
        else if (progress) progress("Querying Overpass API...");
        response = httpPost(overpassServers[attempt], postBody, USER_AGENT);
        // Detect HTML error pages (rate limiting) -- first non-ws char must be { or [
        if (!response.empty()) {
            size_t i = 0;
            while (i < response.size() && (response[i] == ' ' || response[i] == '\t' || response[i] == '\n' || response[i] == '\r')) i++;
            if (i >= response.size() || (response[i] != '{' && response[i] != '[')) {
                if (progress) progress("Overpass returned non-JSON response (rate limited?), retrying...");
                response.clear();
            }
        }
    }
    if (response.empty()) {
        errorMsg = "Overpass API returned empty or non-JSON response (rate limited?)";
        return false;
    }

    if (progress) progress("Parsing seamark data...");

    std::string jsonStr(response.begin(), response.end());
    if (!parseResponse(jsonStr)) {
        return false;
    }

    // Link lights to their closest buoy
    for (auto& light : lights) {
        light.buoyIndex = findClosestBuoy(light.longitude, light.latitude, buoys);
    }

    queryDone = true;
    if (progress) {
        std::ostringstream msg;
        msg << "Found " << buoys.size() << " buoys, "
            << lights.size() << " lights, "
            << landmarks.size() << " landmarks";
        progress(msg.str());
    }

    return true;
}

// ---- OSM tag parsing helpers ----

int OpenSeaMapSource::parseCharacteristic(const std::string& s) {
    if (s == "F") return 1;
    if (s == "Fl") return 2;
    if (s == "LFl") return 3;
    if (s == "Q") return 4;
    if (s == "VQ") return 5;
    if (s == "Iso") return 7;
    if (s == "Oc") return 8;
    return 2; // default Fl
}

int OpenSeaMapSource::parseColour(const std::string& s) {
    if (s == "white") return 1;
    if (s == "red") return 3;
    if (s == "green") return 4;
    if (s == "blue") return 5;
    if (s == "yellow") return 6;
    if (s == "orange") return 11;
    return 1; // default white
}

static int parseLateralCategory(const std::string& s) {
    if (s == "port" || s == "port_hand") return 1;
    if (s == "starboard" || s == "starboard_hand") return 2;
    if (s == "preferred_channel_starboard") return 3;
    if (s == "preferred_channel_port") return 4;
    return 0;
}

static int parseCardinalCategory(const std::string& s) {
    if (s == "north") return 1;
    if (s == "east") return 2;
    if (s == "south") return 3;
    if (s == "west") return 4;
    return 0;
}

static int parseShape(const std::string& s) {
    if (s == "conical") return 1;
    if (s == "can") return 2;
    if (s == "spherical") return 3;
    if (s == "pillar") return 4;
    if (s == "spar") return 5;
    if (s == "barrel") return 6;
    return 0;
}

static int parseTopmarkShape(const std::string& s) {
    if (s == "cone, point up") return 1;
    if (s == "cone, point down") return 2;
    if (s == "sphere") return 3;
    if (s == "2 cones, point up" || s == "2 cones point up") return 4;
    if (s == "2 cones, point down" || s == "2 cones point down") return 5;
    if (s == "2 cones base to base") return 6;
    if (s == "2 cones point to point") return 7;
    if (s == "x-shape") return 8;
    if (s == "2 spheres") return 9;
    return 0;
}

// IALA-A colour defaults when OSM colour tag is absent
static std::string inferBuoyColours(const OsmBuoy& buoy) {
    if (!buoy.colours.empty()) return buoy.colours;

    if (buoy.layerName == "BOYLAT") {
        if (buoy.categoryLateral == 1) return "red";
        if (buoy.categoryLateral == 2) return "green";
        if (buoy.categoryLateral == 3) return "green;red;green";
        if (buoy.categoryLateral == 4) return "red;green;red";
    }
    if (buoy.layerName == "BOYCAR") {
        if (buoy.categoryCardinal == 1) return "black;yellow";
        if (buoy.categoryCardinal == 2) return "black;yellow;black";
        if (buoy.categoryCardinal == 3) return "yellow;black";
        if (buoy.categoryCardinal == 4) return "yellow;black;yellow";
    }
    if (buoy.layerName == "BOYISD") return "black;red;black";
    if (buoy.layerName == "BOYSAW") return "red;white";
    if (buoy.layerName == "BOYSPP") return "yellow";

    return "red";
}

// Extract colour and topmark tags from a buoy element
static void parseBuoyColourAndTopmark(OsmBuoy& buoy, const nlohmann::json& tags,
                                       const std::string& seamarkType) {
    std::string prefix = "seamark:" + seamarkType;
    if (tags.contains(prefix + ":colour"))
        buoy.colours = tags[prefix + ":colour"].get<std::string>();
    if (tags.contains(prefix + ":colour_pattern"))
        buoy.colourPattern = tags[prefix + ":colour_pattern"].get<std::string>();
    if (tags.contains("seamark:topmark:shape"))
        buoy.topmarkShape = parseTopmarkShape(tags["seamark:topmark:shape"].get<std::string>());
    if (tags.contains("seamark:topmark:colour"))
        buoy.topmarkColour = tags["seamark:topmark:colour"].get<std::string>();
}

static int parseLandmarkCategory(const std::string& s) {
    // Map OSM seamark:landmark:category to S-57 CATLMK codes
    if (s == "cairn") return 1;
    if (s == "chimney") return 3;
    if (s == "flagstaff" || s == "flagpole") return 5;
    if (s == "mast") return 7;
    if (s == "monument") return 9;
    if (s == "cross") return 14;
    if (s == "dome") return 15;
    if (s == "tower") return 17;
    if (s == "windmill") return 18;
    if (s == "windmotor" || s == "wind_turbine") return 19;
    if (s == "spire" || s == "minaret") return 20;
    if (s == "church" || s == "chapel") return 20; // spire
    return 0;
}

// ---- JSON parsing ----

bool OpenSeaMapSource::parseResponse(const std::string& jsonStr) {
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
        if (!elem.contains("type")) continue;
        std::string elemType = elem["type"].get<std::string>();
        if (elemType != "node" && elemType != "way") continue;
        if (!elem.contains("tags") || !elem["tags"].is_object())
            continue;

        // Extract lat/lon: nodes have top-level, ways use center sub-object
        double lat = 0, lon = 0;
        if (elem.contains("lat") && elem.contains("lon")) {
            lat = elem["lat"].get<double>();
            lon = elem["lon"].get<double>();
        } else if (elem.contains("center")) {
            const auto& center = elem["center"];
            if (center.contains("lat") && center.contains("lon")) {
                lat = center["lat"].get<double>();
                lon = center["lon"].get<double>();
            } else {
                continue;
            }
        } else {
            continue;
        }

        const auto& tags = elem["tags"];

        std::string seamarkType;
        if (tags.contains("seamark:type"))
            seamarkType = tags["seamark:type"].get<std::string>();

        // ---- Buoys (nodes only, not ways) ----
        if (elemType == "node") {

        if (seamarkType == "buoy_lateral" || seamarkType == "beacon_lateral") {
            OsmBuoy buoy;
            buoy.latitude = lat;
            buoy.longitude = lon;
            buoy.layerName = "BOYLAT";
            buoy.grounded = (seamarkType.find("beacon") != std::string::npos);

            std::string prefix = "seamark:" + seamarkType;
            if (tags.contains(prefix + ":category"))
                buoy.categoryLateral = parseLateralCategory(tags[prefix + ":category"].get<std::string>());
            if (tags.contains(prefix + ":shape"))
                buoy.shape = parseShape(tags[prefix + ":shape"].get<std::string>());
            if (tags.contains("seamark:name"))
                buoy.name = tags["seamark:name"].get<std::string>();
            parseBuoyColourAndTopmark(buoy, tags, seamarkType);

            buoys.push_back(buoy);
        }
        else if (seamarkType == "buoy_cardinal" || seamarkType == "beacon_cardinal") {
            OsmBuoy buoy;
            buoy.latitude = lat;
            buoy.longitude = lon;
            buoy.layerName = "BOYCAR";
            buoy.grounded = (seamarkType.find("beacon") != std::string::npos);

            std::string prefix = "seamark:" + seamarkType;
            if (tags.contains(prefix + ":category"))
                buoy.categoryCardinal = parseCardinalCategory(tags[prefix + ":category"].get<std::string>());
            if (tags.contains(prefix + ":shape"))
                buoy.shape = parseShape(tags[prefix + ":shape"].get<std::string>());
            if (tags.contains("seamark:name"))
                buoy.name = tags["seamark:name"].get<std::string>();
            parseBuoyColourAndTopmark(buoy, tags, seamarkType);

            buoys.push_back(buoy);
        }
        else if (seamarkType == "buoy_isolated_danger" || seamarkType == "beacon_isolated_danger") {
            OsmBuoy buoy;
            buoy.latitude = lat;
            buoy.longitude = lon;
            buoy.layerName = "BOYISD";
            buoy.grounded = (seamarkType.find("beacon") != std::string::npos);
            if (tags.contains("seamark:name"))
                buoy.name = tags["seamark:name"].get<std::string>();
            parseBuoyColourAndTopmark(buoy, tags, seamarkType);
            buoys.push_back(buoy);
        }
        else if (seamarkType == "buoy_safe_water") {
            OsmBuoy buoy;
            buoy.latitude = lat;
            buoy.longitude = lon;
            buoy.layerName = "BOYSAW";
            if (tags.contains("seamark:name"))
                buoy.name = tags["seamark:name"].get<std::string>();
            parseBuoyColourAndTopmark(buoy, tags, seamarkType);
            buoys.push_back(buoy);
        }
        else if (seamarkType == "buoy_special_purpose" || seamarkType == "beacon_special_purpose") {
            OsmBuoy buoy;
            buoy.latitude = lat;
            buoy.longitude = lon;
            buoy.layerName = "BOYSPP";
            buoy.grounded = (seamarkType.find("beacon") != std::string::npos);

            std::string prefix = "seamark:" + seamarkType;
            if (tags.contains(prefix + ":shape"))
                buoy.shape = parseShape(tags[prefix + ":shape"].get<std::string>());
            if (tags.contains("seamark:name"))
                buoy.name = tags["seamark:name"].get<std::string>();
            parseBuoyColourAndTopmark(buoy, tags, seamarkType);

            buoys.push_back(buoy);
        }

        } // end if (elemType == "node") -- buoys only

        // ---- Lights (both nodes and ways) ----
        // Lights can be standalone or on the same node as a buoy.
        // Ways are needed for lighthouses mapped as building outlines.
        if (seamarkType == "light" || seamarkType == "light_major" || seamarkType == "light_minor" ||
            tags.contains("seamark:light:character") || tags.contains("seamark:light:1:character")) {

            // Handle numbered light sectors (seamark:light:1:, seamark:light:2:, ...)
            // Also handle unnumbered (seamark:light:)
            std::vector<std::string> lightPrefixes;

            if (tags.contains("seamark:light:character") || tags.contains("seamark:light:colour")) {
                lightPrefixes.push_back("seamark:light:");
            }
            for (int li = 1; li <= 8; li++) {
                std::string prefix = "seamark:light:" + std::to_string(li) + ":";
                if (tags.contains(prefix + "character") || tags.contains(prefix + "colour")) {
                    lightPrefixes.push_back(prefix);
                }
            }

            // If no specific light tags found, create a default light for light_major/light_minor
            if (lightPrefixes.empty() && (seamarkType == "light_major" || seamarkType == "light_minor")) {
                lightPrefixes.push_back("seamark:light:");
            }

            for (const auto& prefix : lightPrefixes) {
                OsmLight light;
                light.latitude = lat;
                light.longitude = lon;

                if (tags.contains(prefix + "character"))
                    light.characteristic = parseCharacteristic(tags[prefix + "character"].get<std::string>());

                if (tags.contains(prefix + "period")) {
                    try { light.period = std::stod(tags[prefix + "period"].get<std::string>()); }
                    catch (...) {}
                }

                if (tags.contains(prefix + "group"))
                    light.group = tags[prefix + "group"].get<std::string>();

                if (tags.contains(prefix + "colour"))
                    light.colour = parseColour(tags[prefix + "colour"].get<std::string>());

                if (tags.contains(prefix + "range")) {
                    try { light.range = std::stod(tags[prefix + "range"].get<std::string>()); }
                    catch (...) {}
                }

                if (tags.contains(prefix + "height")) {
                    try { light.height = std::stod(tags[prefix + "height"].get<std::string>()); }
                    catch (...) {}
                }

                if (tags.contains(prefix + "sector_start")) {
                    try { light.sectorStart = std::stod(tags[prefix + "sector_start"].get<std::string>()); }
                    catch (...) {}
                }

                if (tags.contains(prefix + "sector_end")) {
                    try { light.sectorEnd = std::stod(tags[prefix + "sector_end"].get<std::string>()); }
                    catch (...) {}
                }

                // Default range based on type
                if (seamarkType == "light_major" && light.range <= 5.0)
                    light.range = 15.0;

                lights.push_back(light);
            }
        }

        // ---- Landmarks (both nodes and ways) ----
        if (seamarkType == "landmark") {
            OsmLandmark lm;
            lm.latitude = lat;
            lm.longitude = lon;

            if (tags.contains("seamark:landmark:category"))
                lm.category = parseLandmarkCategory(tags["seamark:landmark:category"].get<std::string>());

            if (tags.contains("seamark:landmark:height")) {
                try { lm.height = std::stod(tags["seamark:landmark:height"].get<std::string>()); }
                catch (...) {}
            }

            if (tags.contains("seamark:name"))
                lm.name = tags["seamark:name"].get<std::string>();
            else if (tags.contains("name"))
                lm.name = tags["name"].get<std::string>();

            landmarks.push_back(lm);
        }

        // ---- Navigational landmarks (towers, churches, chimneys, masts, lighthouses) ----
        // Creates visible land object models. Lighthouses tagged with
        // seamark:type=light_major/light_minor must also get a landmark entry
        // so a visible tower appears (not just an invisible light).
        {
            std::string manMade, building;
            if (tags.contains("man_made"))
                manMade = tags["man_made"].get<std::string>();
            if (tags.contains("building"))
                building = tags["building"].get<std::string>();

            bool isLighthouse = (manMade == "lighthouse");
            // Also treat light_major/light_minor as lighthouse if no man_made tag
            bool isSeamarkLight = (seamarkType == "light_major" || seamarkType == "light_minor");
            if (seamarkType.empty() || isLighthouse || isSeamarkLight) {

            int cat = 0;
            if (manMade == "tower") cat = 17;
            else if (manMade == "chimney") cat = 3;
            else if (manMade == "lighthouse") cat = 99; // custom: maps to Lighthouse model
            else if (manMade == "mast") cat = 18;
            else if (building == "church" || building == "cathedral" || building == "chapel") cat = 20;
            // Seamark light without man_made tag -> assume lighthouse structure
            else if (isSeamarkLight && cat == 0) cat = 99;

            if (cat > 0) {
                OsmLandmark lm;
                lm.latitude = lat;
                lm.longitude = lon;
                lm.category = cat;

                // Check multiple height tags (seamark-specific take priority)
                if (tags.contains("seamark:landmark:height")) {
                    try { lm.height = std::stod(tags["seamark:landmark:height"].get<std::string>()); }
                    catch (...) {}
                } else if (tags.contains("seamark:light:height")) {
                    try { lm.height = std::stod(tags["seamark:light:height"].get<std::string>()); }
                    catch (...) {}
                } else if (tags.contains("height")) {
                    try { lm.height = std::stod(tags["height"].get<std::string>()); }
                    catch (...) {}
                }
                if (tags.contains("seamark:name"))
                    lm.name = tags["seamark:name"].get<std::string>();
                else if (tags.contains("name"))
                    lm.name = tags["name"].get<std::string>();

                landmarks.push_back(lm);
            }
        }
        } // end navigational landmarks scope
    }

    return true;
}

// ---- Proximity matching ----

int OpenSeaMapSource::findClosestBuoy(double lon, double lat,
                                       const std::vector<OsmBuoy>& buoys) {
    int closestIdx = -1;
    double closestDist = 50.0; // metres

    for (size_t i = 0; i < buoys.size(); i++) {
        double dLat = (lat - buoys[i].latitude) * 111320.0;
        double dLon = (lon - buoys[i].longitude) * 111320.0 * cos(lat * M_PI / 180.0);
        double dist = sqrt(dLat * dLat + dLon * dLon);

        if (dist < closestDist) {
            closestDist = dist;
            closestIdx = static_cast<int>(i);
        }
    }

    return closestIdx;
}

// ---- Buoy type mapping (mirrors ChartReader::mapBuoyType) ----

std::string OpenSeaMapSource::mapBuoyType(const OsmBuoy& buoy) {
    // Shape-primary mapping: explicit shape tag takes priority
    switch (buoy.shape) {
        case 1: return "shape_conical";
        case 2: return "shape_can";
        case 3: return "shape_spherical";
        case 4: return "shape_pillar";
        case 5: return "shape_spar";
        case 6: return "shape_barrel";
    }
    // Infer shape from buoy type/category when shape tag absent
    if (buoy.layerName == "BOYLAT") {
        if (buoy.categoryLateral == 1) return "shape_can";      // port = can (IALA A)
        if (buoy.categoryLateral == 2) return "shape_conical";   // stbd = conical (IALA A)
        return "shape_can";
    }
    if (buoy.layerName == "BOYCAR") return "shape_pillar";
    if (buoy.layerName == "BOYISD") return "shape_pillar";
    if (buoy.layerName == "BOYSAW") return "shape_spherical";
    if (buoy.layerName == "BOYSPP") return "shape_can";
    return "shape_can";
}

// ---- Landmark type mapping (mirrors ChartReader::mapLandmarkType) ----

std::string OpenSeaMapSource::mapLandmarkType(int category) {
    switch (category) {
        case 1:  return "Beacon";
        case 3:  return "Chimneys";
        case 5:  return "Flagstaff";
        case 7:  return "Masts";
        case 9:  return "Cross";
        case 14: return "Cross";
        case 15: return "Church";
        case 17: return "Tower";
        case 18: return "Masts";
        case 19: return "Masts";
        case 20: return "Church";
        case 99: return "Lighthouse"; // custom: man_made=lighthouse
        default: return "Tower";
    }
}

// ---- Light sequence (mirrors ChartReader::lightCharacteristicToSequence) ----

std::string OpenSeaMapSource::lightSequence(int litchr, double period,
                                             const std::string& group) {
    if (period <= 0) period = 4.0;
    int totalSlots = static_cast<int>(period * 4);
    if (totalSlots < 4) totalSlots = 4;
    if (totalSlots > 100) totalSlots = 100;

    std::string seq;

    int groupCount = 1;
    if (!group.empty()) {
        for (char c : group) {
            if (c >= '1' && c <= '9') {
                groupCount = c - '0';
                break;
            }
        }
    }

    switch (litchr) {
        case 1: // Fixed
            seq = std::string(totalSlots, 'L');
            break;
        case 2: // Flashing
            if (groupCount == 1) {
                int lightSlots = 4;
                seq = std::string(lightSlots, 'L');
                seq += std::string(totalSlots - lightSlots, 'D');
            } else {
                int flashLen = 2;
                int gapLen = 2;
                for (int i = 0; i < groupCount; i++) {
                    seq += std::string(flashLen, 'L');
                    if (i < groupCount - 1) seq += std::string(gapLen, 'D');
                }
                if (static_cast<int>(seq.length()) < totalSlots)
                    seq += std::string(totalSlots - seq.length(), 'D');
            }
            break;
        case 3: // Long Flash
            {
                int lightSlots = 8;
                seq = std::string(lightSlots, 'L');
                seq += std::string(totalSlots - lightSlots, 'D');
            }
            break;
        case 4: // Quick
            for (int i = 0; i < totalSlots; i++)
                seq += (i % 4 == 0) ? 'L' : 'D';
            break;
        case 5: // Very Quick
            for (int i = 0; i < totalSlots; i++)
                seq += (i % 2 == 0) ? 'L' : 'D';
            break;
        case 7: // Isophase
            {
                int halfSlots = totalSlots / 2;
                seq = std::string(halfSlots, 'L');
                seq += std::string(totalSlots - halfSlots, 'D');
            }
            break;
        case 8: // Occulting
            {
                int darkSlots = 4;
                int lightSlots = totalSlots - darkSlots;
                seq = std::string(lightSlots, 'L');
                seq += std::string(darkSlots, 'D');
            }
            break;
        default:
            seq = std::string(4, 'L');
            seq += std::string(totalSlots - 4, 'D');
            break;
    }

    return seq;
}

// ---- Colour to RGB (mirrors ChartReader::colourToRGB) ----

void OpenSeaMapSource::colourToRGB(int code, int& r, int& g, int& b) {
    switch (code) {
        case 1:  r = 255; g = 255; b = 255; break; // White
        case 3:  r = 255; g = 0;   b = 0;   break; // Red
        case 4:  r = 0;   g = 255; b = 0;   break; // Green
        case 5:  r = 0;   g = 0;   b = 255; break; // Blue
        case 6:  r = 255; g = 255; b = 0;   break; // Yellow
        case 11: r = 255; g = 165; b = 0;   break; // Orange
        default: r = 255; g = 255; b = 255; break;
    }
}

// ---- INI generation (mirrors ChartReader format) ----

std::string OpenSeaMapSource::generateBuoyIni() const {
    std::ostringstream oss;
    oss << "Number=" << buoys.size() << "\n\n";

    for (size_t i = 0; i < buoys.size(); i++) {
        int idx = static_cast<int>(i) + 1;
        std::string type = mapBuoyType(buoys[i]);

        oss << "Type(" << idx << ")=" << type << "\n";
        oss.precision(7);
        oss << std::fixed;
        oss << "Long(" << idx << ")=" << buoys[i].longitude << "\n";
        oss << "Lat(" << idx << ")=" << buoys[i].latitude << "\n";

        if (buoys[i].grounded || buoys[i].shape == 4 || buoys[i].shape == 5) {
            oss << "Grounded(" << idx << ")=1\n";
        }

        // Write colour data for runtime recolouring
        std::string colours = inferBuoyColours(buoys[i]);
        if (!colours.empty())
            oss << "Colours(" << idx << ")=" << colours << "\n";
        if (!buoys[i].colourPattern.empty())
            oss << "ColourPattern(" << idx << ")=" << buoys[i].colourPattern << "\n";

        oss << "\n";
    }

    return oss.str();
}

std::string OpenSeaMapSource::generateLightIni() const {
    // Include ALL lights: buoy-attached (Buoy=N) and standalone (Buoy=0 with lat/lon)
    std::ostringstream oss;
    oss << "Number=" << lights.size() << "\n\n";

    for (size_t n = 0; n < lights.size(); n++) {
        const OsmLight& light = lights[n];
        int idx = static_cast<int>(n) + 1;

        std::string seq = lightSequence(light.characteristic, light.period, light.group);

        int r, g, b;
        colourToRGB(light.colour, r, g, b);

        if (light.buoyIndex >= 0) {
            // Buoy-attached light
            oss << "Buoy(" << idx << ")=" << (light.buoyIndex + 1) << "\n";
        } else {
            // Standalone light (lighthouse, shore light, etc.)
            oss << "Buoy(" << idx << ")=0\n";
            oss << std::fixed;
            oss.precision(7);
            oss << "Long(" << idx << ")=" << light.longitude << "\n";
            oss << "Lat(" << idx << ")=" << light.latitude << "\n";
        }
        oss << "Sequence(" << idx << ")=" << seq << "\n";
        oss.precision(1);
        oss << std::fixed;
        oss << "Range(" << idx << ")=" << light.range << "\n";
        oss << "PhaseStart(" << idx << ")=" << idx << "\n";
        oss << "Height(" << idx << ")=" << light.height << "\n";
        oss << "Absolute(" << idx << ")=2\n";
        oss << "Red(" << idx << ")=" << r << "\n";
        oss << "Green(" << idx << ")=" << g << "\n";
        oss << "Blue(" << idx << ")=" << b << "\n";

        if (light.sectorStart == 0.0 && light.sectorEnd >= 360.0) {
            oss << "StartAngle(" << idx << ")=0\n";
            oss << "EndAngle(" << idx << ")=360\n";
        } else {
            oss << "StartAngle(" << idx << ")=" << light.sectorStart << "\n";
            oss << "EndAngle(" << idx << ")=" << light.sectorEnd << "\n";
        }

        oss << "\n";
    }

    return oss.str();
}

std::string OpenSeaMapSource::generateLandObjectIni() const {
    std::ostringstream oss;
    oss << "Number=" << landmarks.size() << "\n\n";

    for (size_t i = 0; i < landmarks.size(); i++) {
        int idx = static_cast<int>(i) + 1;
        std::string type = mapLandmarkType(landmarks[i].category);

        oss << "Type(" << idx << ")=" << type << "\n";
        oss.precision(7);
        oss << std::fixed;
        oss << "Long(" << idx << ")=" << landmarks[i].longitude << "\n";
        oss << "Lat(" << idx << ")=" << landmarks[i].latitude << "\n";

        if (landmarks[i].category == 99) {
            // Lighthouse: clamp to sea level, write structural height for procedural model
            oss << "Absolute(" << idx << ")=2\n";
            if (landmarks[i].height > 0) {
                oss.precision(1);
                oss << "HeightAbove(" << idx << ")=" << landmarks[i].height << "\n";
            }
        } else if (landmarks[i].height > 0) {
            oss.precision(1);
            oss << "HeightCorrection(" << idx << ")=" << landmarks[i].height << "\n";
        }

        oss << "Rotation(" << idx << ")=0\n";
        oss << "\n";
    }

    return oss.str();
}
