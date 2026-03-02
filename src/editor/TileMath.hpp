#pragma once

#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace TileMath {

    struct LatLon {
        double lat;
        double lon;
    };

    struct PixelPos {
        int x;
        int y;
    };

    // Clamp latitude to valid Mercator range
    inline double clampLat(double lat) {
        if (lat < -85.0511) return -85.0511;
        if (lat > 85.0511) return 85.0511;
        return lat;
    }

    // Convert longitude to tile X coordinate at given zoom level
    inline int lonToTileX(double lon, int zoom) {
        return static_cast<int>(std::floor((lon + 180.0) / 360.0 * (1 << zoom)));
    }

    // Convert latitude to tile Y coordinate at given zoom level
    inline int latToTileY(double lat, int zoom) {
        double latRad = clampLat(lat) * M_PI / 180.0;
        int result = static_cast<int>(std::floor(
            (1.0 - std::log(std::tan(latRad) + 1.0 / std::cos(latRad)) / M_PI)
            / 2.0 * (1 << zoom)));
        return std::max(0, std::min((1 << zoom) - 1, result));
    }

    // Convert tile X back to longitude (left edge of tile)
    inline double tileXToLon(int x, int zoom) {
        return x / static_cast<double>(1 << zoom) * 360.0 - 180.0;
    }

    // Convert tile Y back to latitude (top edge of tile)
    inline double tileYToLat(int y, int zoom) {
        double n = M_PI - 2.0 * M_PI * y / static_cast<double>(1 << zoom);
        return 180.0 / M_PI * std::atan(0.5 * (std::exp(n) - std::exp(-n)));
    }

    // Convert lat/lon to global pixel coordinates at given zoom
    inline double lonToPixelX(double lon, int zoom, int tileSize = 256) {
        return (lon + 180.0) / 360.0 * (1 << zoom) * tileSize;
    }

    inline double latToPixelY(double lat, int zoom, int tileSize = 256) {
        double latRad = clampLat(lat) * M_PI / 180.0;
        return (1.0 - std::log(std::tan(latRad) + 1.0 / std::cos(latRad)) / M_PI)
            / 2.0 * (1 << zoom) * tileSize;
    }

    // Convert screen pixel position to lat/lon given map state
    inline LatLon pixelToLatLon(double centerLat, double centerLon, int zoom,
                                int pixelX, int pixelY, int tileSize = 256) {
        double centerPixelX = lonToPixelX(centerLon, zoom, tileSize);
        double centerPixelY = latToPixelY(centerLat, zoom, tileSize);

        double globalX = centerPixelX + pixelX;
        double globalY = centerPixelY + pixelY;

        double lon = globalX / ((1 << zoom) * tileSize) * 360.0 - 180.0;
        double n = M_PI - 2.0 * M_PI * globalY / ((1 << zoom) * tileSize);
        double lat = 180.0 / M_PI * std::atan(0.5 * (std::exp(n) - std::exp(-n)));

        return {lat, lon};
    }

    // Inverse: lat/lon to screen pixel offset from map center
    inline PixelPos latLonToPixel(double centerLat, double centerLon, int zoom,
                                  double lat, double lon, int tileSize = 256) {
        double centerPixelX = lonToPixelX(centerLon, zoom, tileSize);
        double centerPixelY = latToPixelY(centerLat, zoom, tileSize);

        double targetPixelX = lonToPixelX(lon, zoom, tileSize);
        double targetPixelY = latToPixelY(lat, zoom, tileSize);

        return {
            static_cast<int>(std::round(targetPixelX - centerPixelX)),
            static_cast<int>(std::round(targetPixelY - centerPixelY))
        };
    }

} // namespace TileMath
