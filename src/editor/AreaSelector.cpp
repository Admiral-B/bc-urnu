#include "AreaSelector.hpp"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

bool AreaSelector::handleClick(double lat, double lon) {
    if (state == IDLE)
        return false;

    if (state == DRAWING) {
        if (!hasFirstCorner) {
            // First click: set first corner
            corner1Lat = lat;
            corner1Lon = lon;
            corner2Lat = lat;
            corner2Lon = lon;
            hasFirstCorner = true;
            return true;
        } else {
            // Second click: finalize
            corner2Lat = lat;
            corner2Lon = lon;
            state = COMPLETE;
            return true;
        }
    }

    return false;
}

void AreaSelector::updatePreview(double lat, double lon) {
    if (state == DRAWING && hasFirstCorner) {
        corner2Lat = lat;
        corner2Lon = lon;
    }
}

double AreaSelector::getWidthKm() const {
    if (state == IDLE)
        return 0;
    double midLat = (getMinLat() + getMaxLat()) / 2.0;
    double lonDiff = getMaxLon() - getMinLon();
    // 1 degree of longitude = cos(lat) * 111.32 km
    return std::abs(lonDiff) * std::cos(midLat * M_PI / 180.0) * 111.32;
}

double AreaSelector::getHeightKm() const {
    if (state == IDLE)
        return 0;
    double latDiff = getMaxLat() - getMinLat();
    // 1 degree of latitude = ~110.574 km
    return std::abs(latDiff) * 110.574;
}
