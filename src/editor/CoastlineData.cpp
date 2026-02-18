#include "CoastlineData.hpp"
#include <fstream>
#include <cstring>

bool CoastlineData::load(const std::string& path) {
    polygons.clear();
    filteredIndices_.clear();
    useFiltered_ = false;

    std::ifstream file(path, std::ios::binary);
    if (!file) return false;

    uint32_t polygonCount = 0;
    file.read(reinterpret_cast<char*>(&polygonCount), 4);
    if (!file || polygonCount > 100000) return false; // sanity check

    polygons.resize(polygonCount);

    for (uint32_t i = 0; i < polygonCount; i++) {
        Polygon& poly = polygons[i];

        // Bounding box
        file.read(reinterpret_cast<char*>(&poly.minLon), 4);
        file.read(reinterpret_cast<char*>(&poly.maxLon), 4);
        file.read(reinterpret_cast<char*>(&poly.minLat), 4);
        file.read(reinterpret_cast<char*>(&poly.maxLat), 4);

        // Vertex count
        uint32_t vertexCount = 0;
        file.read(reinterpret_cast<char*>(&vertexCount), 4);
        if (!file || vertexCount > 10000000) return false;

        // Vertex data (lon, lat pairs as float32)
        poly.vertices.resize(vertexCount * 2);
        file.read(reinterpret_cast<char*>(poly.vertices.data()),
                  vertexCount * 2 * sizeof(float));

        if (!file) {
            polygons.clear();
            return false;
        }
    }

    return true;
}

void CoastlineData::prefilter(double minLon, double maxLon, double minLat, double maxLat) {
    filteredIndices_.clear();
    float fMinLon = static_cast<float>(minLon);
    float fMaxLon = static_cast<float>(maxLon);
    float fMinLat = static_cast<float>(minLat);
    float fMaxLat = static_cast<float>(maxLat);

    for (size_t i = 0; i < polygons.size(); i++) {
        const auto& poly = polygons[i];
        // Check if polygon bbox overlaps the area of interest
        if (poly.maxLon >= fMinLon && poly.minLon <= fMaxLon &&
            poly.maxLat >= fMinLat && poly.minLat <= fMaxLat) {
            filteredIndices_.push_back(i);
        }
    }
    useFiltered_ = true;
}

bool CoastlineData::isLand(double lon, double lat) const {
    float flon = static_cast<float>(lon);
    float flat = static_cast<float>(lat);

    // Use filtered subset if prefilter() was called
    if (useFiltered_) {
        for (size_t idx : filteredIndices_) {
            const auto& poly = polygons[idx];
            // Bounding box pre-filter (still needed for per-point rejection)
            if (flon < poly.minLon || flon > poly.maxLon ||
                flat < poly.minLat || flat > poly.maxLat)
                continue;

            int n = static_cast<int>(poly.vertices.size()) / 2;
            if (n < 3) continue;

            bool inside = false;
            for (int i = 0, j = n - 1; i < n; j = i++) {
                float xi = poly.vertices[i * 2];
                float yi = poly.vertices[i * 2 + 1];
                float xj = poly.vertices[j * 2];
                float yj = poly.vertices[j * 2 + 1];

                if (((yi > flat) != (yj > flat)) &&
                    (flon < (xj - xi) * (flat - yi) / (yj - yi) + xi)) {
                    inside = !inside;
                }
            }
            if (inside) return true;
        }
        return false;
    }

    // Fallback: check all polygons
    for (const auto& poly : polygons) {
        if (flon < poly.minLon || flon > poly.maxLon ||
            flat < poly.minLat || flat > poly.maxLat)
            continue;

        int n = static_cast<int>(poly.vertices.size()) / 2;
        if (n < 3) continue;

        bool inside = false;
        for (int i = 0, j = n - 1; i < n; j = i++) {
            float xi = poly.vertices[i * 2];
            float yi = poly.vertices[i * 2 + 1];
            float xj = poly.vertices[j * 2];
            float yj = poly.vertices[j * 2 + 1];

            if (((yi > flat) != (yj > flat)) &&
                (flon < (xj - xi) * (flat - yi) / (yj - yi) + xi)) {
                inside = !inside;
            }
        }

        if (inside) return true;
    }

    return false;
}
