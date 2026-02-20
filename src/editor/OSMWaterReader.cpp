#include "OSMWaterReader.hpp"
#include "OSMBuildingReader.hpp" // reuse httpPost
#include "../libs/nlohmann/json.hpp"

#include <sstream>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <cmath>

using json = nlohmann::json;

static const char* OVERPASS_URL = "https://overpass-api.de/api/interpreter";
static const char* USER_AGENT = "BridgeCommand/6.0 (world-generator)";

// ---- Classification ----

std::string OSMWaterReader::classifyWaterType(const std::string& natural,
                                               const std::string& waterTag,
                                               const std::string& waterway) {
    if (!waterTag.empty()) {
        if (waterTag == "lake" || waterTag == "pond") return "lake";
        if (waterTag == "river" || waterTag == "canal") return "river";
        if (waterTag == "reservoir" || waterTag == "basin") return "reservoir";
        if (waterTag == "harbour" || waterTag == "dock" || waterTag == "port") return "dock";
    }
    if (!waterway.empty()) {
        if (waterway == "dock") return "dock";
        if (waterway == "riverbank") return "river";
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

    // Query water bodies: ways and relations with full geometry
    // natural=water covers lakes, reservoirs, ponds, harbours, docks, basins
    // natural=bay covers bays
    // waterway=riverbank covers wide rivers
    // waterway=dock covers dock basins
    // landuse=reservoir covers reservoirs tagged via landuse
    std::ostringstream ql;
    ql << std::fixed;
    ql.precision(6);
    ql << "[out:json][timeout:120];"
       << "("
       << "way[\"natural\"=\"water\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon << ");"
       << "relation[\"natural\"=\"water\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon << ");"
       << "way[\"natural\"=\"bay\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon << ");"
       << "relation[\"natural\"=\"bay\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon << ");"
       << "way[\"waterway\"=\"riverbank\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon << ");"
       << "way[\"waterway\"=\"dock\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon << ");"
       << "relation[\"waterway\"=\"riverbank\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon << ");"
       << "way[\"landuse\"=\"reservoir\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon << ");"
       << "way[\"waterway\"=\"dam\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon << ");"
       << "way[\"man_made\"=\"breakwater\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon << ");"
       << ");"
       << "out geom;";

    std::string postBody = "data=" + ql.str();

    if (progress) progress("Querying Overpass API for water areas...");

    auto response = OSMBuildingReader::httpPost(OVERPASS_URL, postBody, USER_AGENT);
    if (response.empty()) {
        errorMsg = "Overpass API returned empty response for water query";
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
            // Multipolygon relation: extract outer members
            if (!elem.contains("members") || !elem["members"].is_array()) continue;

            for (const auto& member : elem["members"]) {
                if (!member.contains("role") || !member.contains("type")) continue;
                std::string role = member["role"].get<std::string>();
                std::string mtype = member["type"].get<std::string>();

                // Only outer rings define water area boundaries
                if (role != "outer" || mtype != "way") continue;
                if (!member.contains("geometry") || !member["geometry"].is_array()) continue;

                const auto& geom = member["geometry"];
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
