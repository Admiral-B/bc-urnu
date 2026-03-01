#include "GBATileDownloader.hpp"
#include "WinHTTPConnectionPool.hpp"

#include <fstream>
#include <sstream>
#include <filesystem>
#include <cmath>
#include <algorithm>
#include <iostream>

#include "../libs/nlohmann/json.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

GBATileDownloader::GBATileDownloader(const std::string& cacheDir)
    : cacheDir(cacheDir) {
    try { fs::create_directories(cacheDir); } catch (...) {}
}

std::string GBATileDownloader::tileName(double lat, double lon) {
    // GBA tiles are 5x5 degrees. Tile boundaries align to multiples of 5.
    // Name format: "{ew}{absLonW}_{ns}{absLatN}_{ew}{absLonE}_{ns}{absLatS}"
    // Example: lat=52, lon=-3 -> tile covers [-5,0] x [50,55] -> "w5_n55_w0_n50"
    //   Wait, re-reading the MEMORY.md example: "w5_n55_w0_n50" for UK
    //   That's: west=5W, north=55N, east=0, south=50N
    //   So the format is: {lonW}_{latN}_{lonE}_{latS} where each is like "w5" or "e10" or "n55" or "s10"

    // Compute the 5-degree cell containing this point
    int lonCell = (int)std::floor(lon / 5.0) * 5;
    int latCell = (int)std::floor(lat / 5.0) * 5;

    int lonW = lonCell;
    int lonE = lonCell + 5;
    int latS = latCell;
    int latN = latCell + 5;

    auto lonStr = [](int v) -> std::string {
        if (v < 0) return "w" + std::to_string(-v);
        if (v > 0) return "e" + std::to_string(v);
        return "w0"; // 0 is conventionally w0
    };
    auto latStr = [](int v) -> std::string {
        if (v < 0) return "s" + std::to_string(-v);
        return "n" + std::to_string(v);
    };

    return lonStr(lonW) + "_" + latStr(latN) + "_" + lonStr(lonE) + "_" + latStr(latS);
}

std::vector<std::string> GBATileDownloader::tileNamesForBounds(
    double minLat, double maxLat, double minLon, double maxLon) {
    std::vector<std::string> names;

    int lonStart = (int)std::floor(minLon / 5.0) * 5;
    int lonEnd = (int)std::floor(maxLon / 5.0) * 5;
    int latStart = (int)std::floor(minLat / 5.0) * 5;
    int latEnd = (int)std::floor(maxLat / 5.0) * 5;

    for (int lat = latStart; lat <= latEnd; lat += 5) {
        for (int lon = lonStart; lon <= lonEnd; lon += 5) {
            names.push_back(tileName((double)lat + 2.5, (double)lon + 2.5));
        }
    }
    return names;
}

std::string GBATileDownloader::buildHuggingFaceUrl(const std::string& name) {
    // HuggingFace dataset: zhu-xlab/GBA.ODbLPolygon
    return "https://huggingface.co/datasets/zhu-xlab/GBA.ODbLPolygon/resolve/main/"
           + name + ".geojson";
}

std::string GBATileDownloader::tileCachePath(const std::string& name) const {
    return cacheDir + "/" + name + ".geojson";
}

bool GBATileDownloader::downloadTile(const std::string& name, ProgressCallback progress) {
    std::string cachePath = tileCachePath(name);

    // Check disk cache
    if (fs::exists(cachePath)) {
        if (progress) progress("Loading cached GBA tile: " + name);
        std::ifstream f(cachePath);
        if (f) {
            std::ostringstream ss;
            ss << f.rdbuf();
            return parseGeoJSON(ss.str());
        }
    }

    // Download from HuggingFace
    if (progress) progress("Downloading GBA tile: " + name);

    std::string url = buildHuggingFaceUrl(name);
    WinHTTPConnectionPool pool("BridgeCommand/6.0 (gba-downloader)");
    auto data = pool.get(url);

    if (data.empty()) {
        if (progress) progress("GBA tile not available: " + name);
        return false;
    }

    // Save to cache
    try {
        fs::create_directories(fs::path(cachePath).parent_path());
        std::ofstream f(cachePath, std::ios::binary);
        f.write(reinterpret_cast<const char*>(data.data()), data.size());
    } catch (...) {}

    std::string jsonStr(data.begin(), data.end());
    return parseGeoJSON(jsonStr);
}

bool GBATileDownloader::parseGeoJSON(const std::string& jsonStr) {
    try {
        auto doc = json::parse(jsonStr);
        if (!doc.contains("features") || !doc["features"].is_array()) return false;

        for (auto& feature : doc["features"]) {
            if (!feature.contains("geometry") || !feature.contains("properties")) continue;

            auto& geom = feature["geometry"];
            if (geom.value("type", "") != "Polygon") continue;
            if (!geom.contains("coordinates") || !geom["coordinates"].is_array()) continue;
            if (geom["coordinates"].empty()) continue;

            auto& props = feature["properties"];
            GBABuilding bld;
            bld.height = props.value("height", 0.0f);
            bld.variance = props.value("var", 0.0f);

            // Parse outer ring (first ring of Polygon coordinates)
            auto& ring = geom["coordinates"][0];
            double sumX = 0.0, sumY = 0.0;
            for (auto& coord : ring) {
                if (coord.is_array() && coord.size() >= 2) {
                    double x = coord[0].get<double>();
                    double y = coord[1].get<double>();
                    bld.outline.push_back({x, y});
                    sumX += x;
                    sumY += y;
                }
            }

            if (bld.outline.size() < 3) continue;

            bld.centroidX = sumX / bld.outline.size();
            bld.centroidY = sumY / bld.outline.size();

            buildings.push_back(std::move(bld));
        }

        buildSpatialIndex();
        return true;
    } catch (const std::exception& e) {
        std::cerr << "GBA GeoJSON parse error: " << e.what() << std::endl;
        return false;
    }
}

bool GBATileDownloader::loadForBounds(double minLat, double maxLat,
                                       double minLon, double maxLon,
                                       ProgressCallback progress) {
    buildings.clear();
    spatialGrid.clear();

    auto names = tileNamesForBounds(minLat, maxLat, minLon, maxLon);
    bool anyLoaded = false;
    for (auto& name : names) {
        if (downloadTile(name, progress)) {
            anyLoaded = true;
        }
    }
    return anyLoaded;
}

void GBATileDownloader::buildSpatialIndex() {
    spatialGrid.clear();
    for (int i = 0; i < (int)buildings.size(); i++) {
        int64_t key = gridKey(buildings[i].centroidX, buildings[i].centroidY);
        spatialGrid[key].push_back(i);
    }
}

int64_t GBATileDownloader::gridKey(double x, double y) const {
    int32_t gx = (int32_t)std::floor(x / CELL_SIZE);
    int32_t gy = (int32_t)std::floor(y / CELL_SIZE);
    return ((int64_t)gx << 32) | (uint32_t)gy;
}

std::vector<int64_t> GBATileDownloader::neighborKeys(double x, double y, double radius) const {
    int32_t gxMin = (int32_t)std::floor((x - radius) / CELL_SIZE);
    int32_t gxMax = (int32_t)std::floor((x + radius) / CELL_SIZE);
    int32_t gyMin = (int32_t)std::floor((y - radius) / CELL_SIZE);
    int32_t gyMax = (int32_t)std::floor((y + radius) / CELL_SIZE);

    std::vector<int64_t> keys;
    for (int32_t gx = gxMin; gx <= gxMax; gx++) {
        for (int32_t gy = gyMin; gy <= gyMax; gy++) {
            keys.push_back(((int64_t)gx << 32) | (uint32_t)gy);
        }
    }
    return keys;
}

const GBATileDownloader::GBABuilding*
GBATileDownloader::findNearest(double x3857, double y3857, double searchRadiusM) const {
    auto keys = neighborKeys(x3857, y3857, searchRadiusM);
    const GBABuilding* best = nullptr;
    double bestDist2 = searchRadiusM * searchRadiusM;

    for (auto key : keys) {
        auto it = spatialGrid.find(key);
        if (it == spatialGrid.end()) continue;
        for (int idx : it->second) {
            double dx = buildings[idx].centroidX - x3857;
            double dy = buildings[idx].centroidY - y3857;
            double d2 = dx * dx + dy * dy;
            if (d2 < bestDist2) {
                bestDist2 = d2;
                best = &buildings[idx];
            }
        }
    }
    return best;
}

const GBATileDownloader::GBABuilding*
GBATileDownloader::findContaining(double x3857, double y3857) const {
    // Use spatial index to find candidate buildings, then ray-cast PIP test
    auto keys = neighborKeys(x3857, y3857, CELL_SIZE);

    for (auto key : keys) {
        auto it = spatialGrid.find(key);
        if (it == spatialGrid.end()) continue;
        for (int idx : it->second) {
            auto& bld = buildings[idx];
            // Point-in-polygon (ray casting)
            bool inside = false;
            int n = (int)bld.outline.size();
            for (int i = 0, j = n - 1; i < n; j = i++) {
                double xi = bld.outline[i].first, yi = bld.outline[i].second;
                double xj = bld.outline[j].first, yj = bld.outline[j].second;
                if (((yi > y3857) != (yj > y3857)) &&
                    (x3857 < (xj - xi) * (y3857 - yi) / (yj - yi) + xi)) {
                    inside = !inside;
                }
            }
            if (inside) return &bld;
        }
    }
    return nullptr;
}
