#pragma once

#include <cmath>

// EPSG:4326 (WGS84 lat/lon) <-> EPSG:3857 (Web Mercator) coordinate transforms.
// These match the standard tile server projection used by OSM, ESRI, and GBA data.
namespace CoordinateProjection {

constexpr double EARTH_RADIUS = 6378137.0;       // WGS84 semi-major axis (metres)
constexpr double MAX_LATITUDE = 85.0511287798;    // Web Mercator latitude limit
constexpr double PI = 3.14159265358979323846;

// Convert longitude (degrees) to EPSG:3857 X (metres)
inline double lonToX(double lon) {
    return lon * EARTH_RADIUS * PI / 180.0;
}

// Convert latitude (degrees) to EPSG:3857 Y (metres)
inline double latToY(double lat) {
    double latRad = lat * PI / 180.0;
    return EARTH_RADIUS * std::log(std::tan(PI / 4.0 + latRad / 2.0));
}

// Convert EPSG:3857 X (metres) to longitude (degrees)
inline double xToLon(double x) {
    return x * 180.0 / (EARTH_RADIUS * PI);
}

// Convert EPSG:3857 Y (metres) to latitude (degrees)
inline double yToLat(double y) {
    return (2.0 * std::atan(std::exp(y / EARTH_RADIUS)) - PI / 2.0) * 180.0 / PI;
}

} // namespace CoordinateProjection
