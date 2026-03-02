#pragma once

#include <cmath>
#include <cstdio>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct ImDrawList;
struct ImVec2;

class MeasureTool {
public:
    enum State { IDLE, PLACING_A, PLACING_B, COMPLETE };

    void startMeasuring() { state = PLACING_A; }
    void clear() { state = IDLE; }

    State getState() const { return state; }

    // Handle a map click; returns true if consumed
    bool handleClick(double lat, double lon) {
        if (state == PLACING_A) {
            latA = lat; lonA = lon;
            state = PLACING_B;
            return true;
        } else if (state == PLACING_B) {
            latB = lat; lonB = lon;
            state = COMPLETE;
            return true;
        }
        return false;
    }

    // Update preview endpoint while placing B
    void updatePreview(double lat, double lon) {
        if (state == PLACING_B) {
            latB = lat; lonB = lon;
        }
    }

    // Haversine great-circle distance in NM
    static double haversineNM(double lat1, double lon1, double lat2, double lon2) {
        double dLat = (lat2 - lat1) * M_PI / 180.0;
        double dLon = (lon2 - lon1) * M_PI / 180.0;
        double a = sin(dLat / 2) * sin(dLat / 2) +
                   cos(lat1 * M_PI / 180) * cos(lat2 * M_PI / 180) *
                   sin(dLon / 2) * sin(dLon / 2);
        double c = 2 * atan2(sqrt(a), sqrt(1 - a));
        return 3440.065 * c;
    }

    // Initial bearing from A to B in degrees true (0-360)
    static double bearing(double lat1, double lon1, double lat2, double lon2) {
        double dLon = (lon2 - lon1) * M_PI / 180.0;
        double lat1r = lat1 * M_PI / 180.0;
        double lat2r = lat2 * M_PI / 180.0;
        double y = sin(dLon) * cos(lat2r);
        double x = cos(lat1r) * sin(lat2r) - sin(lat1r) * cos(lat2r) * cos(dLon);
        double brg = atan2(y, x) * 180.0 / M_PI;
        if (brg < 0) brg += 360.0;
        return brg;
    }

    double getDistNM() const { return haversineNM(latA, lonA, latB, lonB); }
    double getDistKm() const { return getDistNM() * 1.852; }
    double getBearing() const { return bearing(latA, lonA, latB, lonB); }

    // Render the measurement line and labels
    using ToPixelFn = ImVec2(*)(double lat, double lon, void* userData);

    void render(ImDrawList* drawList, ToPixelFn toPixel, void* userData, float speed = 10.0f);

    double latA = 0, lonA = 0;
    double latB = 0, lonB = 0;
    float measureSpeed = 10.0f; // Configurable speed for transit time

private:
    State state = IDLE;
};
