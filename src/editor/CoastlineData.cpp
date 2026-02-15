#include "CoastlineData.hpp"
#include <fstream>
#include <cstring>

bool CoastlineData::load(const std::string& path) {
    polygons.clear();

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

bool CoastlineData::isLand(double lon, double lat) const {
    float flon = static_cast<float>(lon);
    float flat = static_cast<float>(lat);

    for (const auto& poly : polygons) {
        // Bounding box pre-filter
        if (flon < poly.minLon || flon > poly.maxLon ||
            flat < poly.minLat || flat > poly.maxLat)
            continue;

        // Ray-casting algorithm
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
