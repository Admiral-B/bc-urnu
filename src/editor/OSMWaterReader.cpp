#include "OSMWaterReader.hpp"
#include "OSMBuildingReader.hpp" // reuse httpPost
#include "../libs/nlohmann/json.hpp"

#include <sstream>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <cmath>
#include <algorithm>

using json = nlohmann::json;

static const char* OVERPASS_URL = "https://overpass-api.de/api/interpreter";
static const char* USER_AGENT = "BridgeCommand/6.0 (world-generator)";

// ---- Ring stitching for multipolygon relations ----
// Multipolygon outer rings may arrive as multiple separate way segments
// that share endpoints. Stitch them into closed rings by greedy endpoint matching.
static std::vector<std::vector<std::pair<double,double>>>
stitchRings(std::vector<std::vector<std::pair<double,double>>>& ways) {
    std::vector<std::vector<std::pair<double,double>>> rings;
    std::vector<bool> used(ways.size(), false);
    const double EPS = 1e-7;

    auto near = [&](const std::pair<double,double>& a, const std::pair<double,double>& b) {
        return std::abs(a.first - b.first) < EPS && std::abs(a.second - b.second) < EPS;
    };

    for (size_t start = 0; start < ways.size(); start++) {
        if (used[start]) continue;
        used[start] = true;

        std::vector<std::pair<double,double>> ring = ways[start];

        // Keep trying to extend until ring closes or no match found
        bool extended = true;
        while (extended && !near(ring.front(), ring.back())) {
            extended = false;
            for (size_t i = 0; i < ways.size(); i++) {
                if (used[i]) continue;
                auto& w = ways[i];
                if (w.empty()) continue;
                if (near(ring.back(), w.front())) {
                    ring.insert(ring.end(), w.begin() + 1, w.end());
                    used[i] = true;
                    extended = true;
                    break;
                } else if (near(ring.back(), w.back())) {
                    ring.insert(ring.end(), w.rbegin() + 1, w.rend());
                    used[i] = true;
                    extended = true;
                    break;
                }
            }
        }
        rings.push_back(std::move(ring));
    }

    return rings;
}

// ---- Classification ----

std::string OSMWaterReader::classifyWaterType(const std::string& natural,
                                               const std::string& waterTag,
                                               const std::string& waterway) {
    if (!waterTag.empty()) {
        if (waterTag == "lock") return "lock"; // navigable lock chamber
        if (waterTag == "lake" || waterTag == "pond") return "lake";
        if (waterTag == "river" || waterTag == "canal") return "river";
        if (waterTag == "reservoir" || waterTag == "basin") return "reservoir";
        if (waterTag == "harbour" || waterTag == "dock" || waterTag == "port" ||
            waterTag == "marina") return "dock";
    }
    if (!waterway.empty()) {
        if (waterway == "lock") return "lock";
        if (waterway == "dock") return "dock";
        if (waterway == "riverbank") return "river";
        if (waterway == "canal") return "river";
    }
    if (natural == "bay") return "sea";
    if (natural == "water") return "lake"; // generic fallback
    return "water";
}

// ---- Point-in-polygon (ray casting) ----

bool OSMWaterReader::pointInRing(double lat, double lon,
                                  const std::vector<std::pair<double, double>>& ring) {
    if (ring.size() < 3) return false;
    bool inside = false;
    size_t n = ring.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        double yi = ring[i].first,  xi = ring[i].second;  // lat, lon
        double yj = ring[j].first,  xj = ring[j].second;
        if (((yi > lat) != (yj > lat)) &&
            (lon < (xj - xi) * (lat - yi) / (yj - yi) + xi)) {
            inside = !inside;
        }
    }
    return inside;
}

bool OSMWaterReader::isWater(double lat, double lon) const {
    for (const auto& wp : waterAreas) {
        if (pointInRing(lat, lon, wp.outline)) return true;
    }
    return false;
}

// ---- Overpass query ----

bool OSMWaterReader::query(double minLat, double maxLat,
                            double minLon, double maxLon,
                            ProgressCallback progress) {
    waterAreas.clear();
    barriers.clear();
    queryDone = false;
    errorMsg.clear();

    if (progress) progress("Building Overpass query for water areas...");

    // Query enclosed water bodies (NOT open sea/bays -- those are already not-land).
    // natural=water: lakes, reservoirs, ponds, harbours, docks, basins
    // waterway=riverbank/dock/canal/lock: rivers, docks, canals, lock chambers
    // harbour: harbour basins
    // landuse=reservoir/basin: reservoirs
    // Note: natural=bay deliberately excluded -- returns entire sea regions
    // (Bristol Channel etc.) which would submerge islands like Flat Holm.
    std::ostringstream ql;
    ql << std::fixed;
    ql.precision(6);
    ql << "[out:json][timeout:120];"
       << "("
       << "way[\"natural\"=\"water\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon << ");"
       << "relation[\"natural\"=\"water\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon << ");"
       << "way[\"waterway\"=\"riverbank\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon << ");"
       << "way[\"waterway\"=\"dock\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon << ");"
       << "way[\"waterway\"=\"canal\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon << ");"
       << "way[\"waterway\"=\"lock\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon << ");"
       << "relation[\"waterway\"=\"lock\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon << ");"
       << "relation[\"waterway\"=\"canal\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon << ");"
       << "way[\"lock\"=\"yes\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon << ");"
       << "way[\"seamark:type\"=\"lock_basin\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon << ");"
       << "relation[\"waterway\"=\"riverbank\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon << ");"
       << "relation[\"waterway\"=\"dock\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon << ");"
       << "way[\"landuse\"=\"reservoir\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon << ");"
       << "way[\"landuse\"=\"basin\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon << ");"
       << "way[\"harbour\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon << ");"
       << "way[\"waterway\"=\"dam\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon << ");"
       << "way[\"man_made\"=\"breakwater\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon << ");"
       << "way[\"leisure\"=\"marina\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon << ");"
       << "relation[\"leisure\"=\"marina\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon << ");"
       << "way[\"seamark:type\"=\"harbour_basin\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon << ");"
       << "way[\"waterway\"=\"boatyard\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon << ");"
       << "way[\"waterway\"=\"mooring\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon << ");"
       << "way[\"natural\"=\"wetland\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon << ");"
       << "way[\"natural\"=\"mud\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon << ");"
       << "way[\"natural\"=\"tidal_flat\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon << ");"
       << ");"
       << "out geom;";

    std::string postBody = "data=" + ql.str();

    const char* servers[] = {
        OVERPASS_URL,
        "https://overpass.kumi.systems/api/interpreter"
    };

    std::vector<uint8_t> response;
    for (auto& server : servers) {
        if (progress) progress(std::string("Querying water areas from ") + server + "...");
        response = OSMBuildingReader::httpPost(server, postBody, USER_AGENT);
        // Detect HTML error pages (rate limiting) -- first non-ws char must be { or [
        if (!response.empty()) {
            size_t i = 0;
            while (i < response.size() && (response[i] == ' ' || response[i] == '\t' || response[i] == '\n' || response[i] == '\r')) i++;
            if (i >= response.size() || (response[i] != '{' && response[i] != '[')) {
                if (progress) progress("Overpass returned non-JSON response (rate limited?), retrying...");
                response.clear();
            }
        }
        if (!response.empty()) break;
    }
    if (response.empty()) {
        errorMsg = "All Overpass servers failed for water query";
        return false;
    }

    if (progress) progress("Parsing water polygons...");

    std::string jsonStr(response.begin(), response.end());
    if (!parseResponse(jsonStr)) {
        return false;
    }

    queryDone = true;
    if (progress) {
        std::ostringstream msg;
        msg << "Found " << waterAreas.size() << " water areas";
        progress(msg.str());
    }
    return true;
}

// ---- JSON parsing ----

bool OSMWaterReader::parseResponse(const std::string& jsonStr) {
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

        // Extract tags for classification
        std::string natural, waterTag, waterway;
        if (elem.contains("tags") && elem["tags"].is_object()) {
            const auto& tags = elem["tags"];
            if (tags.contains("natural"))
                natural = tags["natural"].get<std::string>();
            if (tags.contains("water"))
                waterTag = tags["water"].get<std::string>();
            if (tags.contains("waterway"))
                waterway = tags["waterway"].get<std::string>();
            if (tags.contains("landuse")) {
                std::string lu = tags["landuse"].get<std::string>();
                if (lu == "reservoir") waterTag = "reservoir";
            }
            if (tags.contains("leisure")) {
                std::string leisure = tags["leisure"].get<std::string>();
                if (leisure == "marina") waterTag = "marina";
            }
            if (tags.contains("seamark:type")) {
                std::string smt = tags["seamark:type"].get<std::string>();
                if (smt == "harbour_basin") waterTag = "dock";
                if (smt == "lock_basin") waterTag = "lock";
            }
            // lock=yes attribute on canal/waterway ways
            if (tags.contains("lock")) {
                std::string lockVal = tags["lock"].get<std::string>();
                if (lockVal == "yes") waterway = "lock";
            }
        }

        std::string wtype = classifyWaterType(natural, waterTag, waterway);

        // Check if this element is a barrier (dam or breakwater)
        bool isBarrier = false;
        if (elem.contains("tags") && elem["tags"].is_object()) {
            const auto& tags = elem["tags"];
            if (tags.contains("waterway") && tags["waterway"].get<std::string>() == "dam")
                isBarrier = true;
            if (tags.contains("man_made") && tags["man_made"].get<std::string>() == "breakwater")
                isBarrier = true;
        }

        if (elemType == "way") {
            if (!elem.contains("geometry") || !elem["geometry"].is_array()) continue;
            const auto& geom = elem["geometry"];

            if (isBarrier) {
                // Barrier way: store as polyline (needs at least 2 points)
                if (geom.size() < 2) continue;
                std::vector<std::pair<double,double>> polyline;
                for (const auto& pt : geom) {
                    if (pt.contains("lat") && pt.contains("lon")) {
                        polyline.emplace_back(pt["lat"].get<double>(),
                                              pt["lon"].get<double>());
                    }
                }
                if (polyline.size() >= 2)
                    barriers.push_back(std::move(polyline));
                continue;
            }

            // Simple closed water way
            if (geom.size() < 3) continue;

            WaterPolygon wp;
            wp.type = wtype;
            for (const auto& pt : geom) {
                if (pt.contains("lat") && pt.contains("lon")) {
                    wp.outline.emplace_back(pt["lat"].get<double>(),
                                            pt["lon"].get<double>());
                }
            }
            if (wp.outline.size() >= 3)
                waterAreas.push_back(std::move(wp));

        } else if (elemType == "relation") {
            // Multipolygon relation: collect outer way segments and stitch into rings.
            // Large water bodies (e.g. Cardiff Bay) have multiple outer ways that
            // share endpoints and must be stitched to form a single closed polygon.
            if (!elem.contains("members") || !elem["members"].is_array()) continue;

            std::vector<std::vector<std::pair<double,double>>> outerWays;
            for (const auto& member : elem["members"]) {
                if (!member.contains("role") || !member.contains("type")) continue;
                std::string role = member["role"].get<std::string>();
                std::string mtype = member["type"].get<std::string>();

                if (role != "outer" || mtype != "way") continue;
                if (!member.contains("geometry") || !member["geometry"].is_array()) continue;

                std::vector<std::pair<double,double>> way;
                for (const auto& pt : member["geometry"]) {
                    if (pt.contains("lat") && pt.contains("lon")) {
                        way.emplace_back(pt["lat"].get<double>(),
                                         pt["lon"].get<double>());
                    }
                }
                if (way.size() >= 2)
                    outerWays.push_back(std::move(way));
            }

            // Stitch way segments into closed rings
            auto rings = stitchRings(outerWays);
            for (auto& ring : rings) {
                if (ring.size() >= 3) {
                    WaterPolygon wp;
                    wp.type = wtype;
                    wp.outline = std::move(ring);
                    waterAreas.push_back(std::move(wp));
                }
            }
        }
    }

    return true;
}

// ---- Disk cache ----

bool OSMWaterReader::saveCache(const std::string& path) const {
    std::ofstream f(path);
    if (!f.is_open()) return false;

    f << std::fixed << std::setprecision(8);
    f << waterAreas.size() << "\n";
    for (const auto& wp : waterAreas) {
        f << wp.outline.size() << " " << wp.type << "\n";
        for (const auto& [lat, lon] : wp.outline) {
            f << lat << " " << lon << "\n";
        }
    }
    return true;
}

bool OSMWaterReader::loadCache(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return false;

    waterAreas.clear();
    queryDone = false;

    size_t count = 0;
    if (!(f >> count)) return false;

    waterAreas.reserve(count);
    for (size_t i = 0; i < count; i++) {
        WaterPolygon wp;
        size_t npts = 0;
        if (!(f >> npts >> wp.type)) return false;

        wp.outline.resize(npts);
        for (size_t j = 0; j < npts; j++) {
            if (!(f >> wp.outline[j].first >> wp.outline[j].second))
                return false;
        }
        waterAreas.push_back(std::move(wp));
    }

    queryDone = true;
    return true;
}
