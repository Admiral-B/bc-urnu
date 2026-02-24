#include "OSMLandUseReader.hpp"
#include "OSMBuildingReader.hpp" // reuse httpPost
#include "../libs/nlohmann/json.hpp"

#include <sstream>
#include <iostream>
#include <algorithm>
#include <cmath>

using json = nlohmann::json;

static const char* OVERPASS_URL = "https://overpass-api.de/api/interpreter";
static const char* USER_AGENT = "BridgeCommand/6.0 (world-generator)";

// ---- Ring stitching (same algorithm as OSMWaterReader) ----

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

        bool extended = true;
        while (extended && !near(ring.front(), ring.back())) {
            extended = false;
            for (size_t i = 0; i < ways.size(); i++) {
                if (used[i] || ways[i].empty()) continue;
                if (near(ring.back(), ways[i].front())) {
                    ring.insert(ring.end(), ways[i].begin() + 1, ways[i].end());
                    used[i] = true; extended = true; break;
                } else if (near(ring.back(), ways[i].back())) {
                    ring.insert(ring.end(), ways[i].rbegin() + 1, ways[i].rend());
                    used[i] = true; extended = true; break;
                }
            }
        }
        rings.push_back(std::move(ring));
    }
    return rings;
}

// ---- Classification ----

LandUseType OSMLandUseReader::classify(const std::string& landuse,
                                        const std::string& natural,
                                        const std::string& leisure) {
    // landuse tags
    if (landuse == "forest") return LandUseType::Forest;
    if (landuse == "residential") return LandUseType::Residential;
    if (landuse == "industrial") return LandUseType::Industrial;
    if (landuse == "commercial" || landuse == "retail") return LandUseType::Commercial;
    if (landuse == "farmland" || landuse == "farmyard" ||
        landuse == "orchard" || landuse == "vineyard") return LandUseType::Farmland;
    if (landuse == "grass" || landuse == "meadow" ||
        landuse == "village_green" || landuse == "recreation_ground") return LandUseType::Grass;
    if (landuse == "construction") return LandUseType::Construction;
    if (landuse == "quarry") return LandUseType::Quarry;
    if (landuse == "allotments") return LandUseType::Allotments;
    if (landuse == "cemetery") return LandUseType::Cemetery;
    if (landuse == "parking") return LandUseType::Parking;

    // natural tags
    if (natural == "wood") return LandUseType::Forest;
    if (natural == "heath" || natural == "scrub" || natural == "moor") return LandUseType::Heath;
    if (natural == "beach" || natural == "sand") return LandUseType::Beach;
    if (natural == "rock" || natural == "cliff" || natural == "scree" ||
        natural == "bare_rock") return LandUseType::Rock;
    if (natural == "grassland") return LandUseType::Grass;
    if (natural == "wetland") return LandUseType::Wetland;
    if (natural == "mud") return LandUseType::Mud;
    if (natural == "shingle") return LandUseType::Shingle;
    if (natural == "tidal_flat") return LandUseType::TidalFlat;

    // leisure tags
    if (leisure == "park" || leisure == "garden" || leisure == "pitch" ||
        leisure == "golf_course") return LandUseType::Grass;
    if (leisure == "playground" || leisure == "sports_centre" ||
        leisure == "swimming_pool") return LandUseType::Grass;

    // amenity tags (handled via leisure parameter for simplicity)
    // amenity=parking is passed as leisure="parking" from parseResponse

    return LandUseType::Unclassified;
}

// ---- Query ----

bool OSMLandUseReader::query(double minLat, double maxLat,
                              double minLon, double maxLon,
                              ProgressCallback progress) {
    if (progress) progress("Querying OSM for land use data...");

    std::ostringstream ql;
    ql << std::fixed;
    ql.precision(6);
    ql << "[out:json][timeout:120];"
       << "(way[\"landuse\"~\"forest|residential|industrial|commercial|retail|farmland|farmyard|grass|meadow|village_green|recreation_ground|orchard|vineyard|construction|quarry|allotments|cemetery|parking\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon << ");"
       << "relation[\"landuse\"~\"forest|residential|industrial|commercial|construction|quarry\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon << ");"
       << "way[\"natural\"~\"wood|scrub|heath|moor|beach|sand|rock|cliff|scree|bare_rock|grassland|wetland|mud|shingle|tidal_flat\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon << ");"
       << "relation[\"natural\"~\"wood|scrub|heath|wetland\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon << ");"
       << "way[\"leisure\"~\"park|garden|pitch|golf_course|playground|sports_centre|swimming_pool\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon << ");"
       << "way[\"amenity\"=\"parking\"]("
       << minLat << "," << minLon << "," << maxLat << "," << maxLon << "););"
       << "out geom;";

    std::string postBody = "data=" + ql.str();

    const char* servers[] = {
        OVERPASS_URL,
        "https://overpass.kumi.systems/api/interpreter"
    };

    std::vector<uint8_t> response;
    for (auto& server : servers) {
        if (progress) progress(std::string("Querying land use from ") + server + "...");
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
        errorMsg = "All Overpass servers failed for land use query";
        return false;
    }

    std::string jsonStr(response.begin(), response.end());
    if (!parseResponse(jsonStr)) return false;

    queryDone = true;
    if (progress) progress("Land use: " + std::to_string(polygons.size()) + " polygons");
    return true;
}

// ---- JSON parsing ----

bool OSMLandUseReader::parseResponse(const std::string& jsonStr) {
    json root;
    try {
        root = json::parse(jsonStr);
    } catch (const std::exception& e) {
        errorMsg = std::string("JSON parse error: ") + e.what();
        return false;
    }

    if (!root.contains("elements") || !root["elements"].is_array()) {
        errorMsg = "No 'elements' in response";
        return false;
    }

    polygons.clear();

    for (const auto& elem : root["elements"]) {
        if (!elem.contains("type")) continue;
        std::string elemType = elem["type"].get<std::string>();

        // Extract tags
        std::string landuse, natural, leisure;
        if (elem.contains("tags") && elem["tags"].is_object()) {
            const auto& tags = elem["tags"];
            if (tags.contains("landuse")) landuse = tags["landuse"].get<std::string>();
            if (tags.contains("natural")) natural = tags["natural"].get<std::string>();
            if (tags.contains("leisure")) leisure = tags["leisure"].get<std::string>();
            // Map amenity=parking -> landuse=parking for classification
            if (landuse.empty() && tags.contains("amenity")) {
                std::string amenity = tags["amenity"].get<std::string>();
                if (amenity == "parking") landuse = "parking";
            }
        }

        LandUseType luType = classify(landuse, natural, leisure);
        if (luType == LandUseType::Unclassified) continue;

        if (elemType == "way") {
            if (!elem.contains("geometry") || !elem["geometry"].is_array()) continue;
            const auto& geom = elem["geometry"];
            if (geom.size() < 3) continue;

            LandUsePolygon lup;
            lup.type = luType;
            for (const auto& pt : geom) {
                if (pt.contains("lat") && pt.contains("lon")) {
                    lup.outline.emplace_back(pt["lat"].get<double>(),
                                              pt["lon"].get<double>());
                }
            }
            if (lup.outline.size() >= 3)
                polygons.push_back(std::move(lup));

        } else if (elemType == "relation") {
            if (!elem.contains("members") || !elem["members"].is_array()) continue;

            std::vector<std::vector<std::pair<double,double>>> outerWays;
            for (const auto& member : elem["members"]) {
                if (!member.contains("role") || !member.contains("type")) continue;
                if (member["role"].get<std::string>() != "outer") continue;
                if (member["type"].get<std::string>() != "way") continue;
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

            auto rings = stitchRings(outerWays);
            for (auto& ring : rings) {
                if (ring.size() >= 3) {
                    LandUsePolygon lup;
                    lup.type = luType;
                    lup.outline = std::move(ring);
                    polygons.push_back(std::move(lup));
                }
            }
        }
    }

    return true;
}

// ---- Rasterization ----

void OSMLandUseReader::rasterize(uint8_t* grid, int resolution,
                                  double minLon, double maxLon,
                                  double minLat, double maxLat) const {
    if (!grid || resolution <= 0) return;

    double lonRange = maxLon - minLon;
    double latRange = maxLat - minLat;
    if (lonRange <= 0 || latRange <= 0) return;

    for (const auto& lup : polygons) {
        if (lup.outline.size() < 3) continue;
        uint8_t val = static_cast<uint8_t>(lup.type);

        // Scanline rasterization
        for (int py = 0; py < resolution; py++) {
            double lat = maxLat - latRange * py / (resolution - 1);

            std::vector<double> crossings;
            size_t n = lup.outline.size();
            for (size_t i = 0, j = n - 1; i < n; j = i++) {
                double yi = lup.outline[i].first, yj = lup.outline[j].first;
                if ((yi > lat) != (yj > lat)) {
                    double xi = lup.outline[i].second, xj = lup.outline[j].second;
                    crossings.push_back(xj + (lat - yj) / (yi - yj) * (xi - xj));
                }
            }
            std::sort(crossings.begin(), crossings.end());

            for (size_t c = 0; c + 1 < crossings.size(); c += 2) {
                int px0 = std::max(0, (int)((crossings[c] - minLon) / lonRange * (resolution - 1)));
                int px1 = std::min(resolution - 1, (int)((crossings[c+1] - minLon) / lonRange * (resolution - 1)));
                for (int px = px0; px <= px1; px++) {
                    grid[py * resolution + px] = val;
                }
            }
        }
    }
}
