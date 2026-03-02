#include "CoastlineRenderer.hpp"
#include "MapWidget.hpp"
#include "TileMath.hpp"
#include <irrlicht.h>

bool CoastlineRenderer::load(const std::string& path50m, const std::string& path10m) {
    bool ok50 = data50m.load(path50m);
    bool ok10 = data10m.load(path10m);
    return ok50 || ok10;
}

void CoastlineRenderer::render(irr::video::IVideoDriver* driver,
                                const MapWidget& mapWidget) {
    int zoom = mapWidget.getZoom();

    if (zoom >= 8 && data10m.isLoaded()) {
        // High zoom: use detailed 10m data, no vertex skipping
        renderPolygons(driver, mapWidget, data10m, 1);
    } else if (data50m.isLoaded()) {
        // Low zoom: use coarse 50m data
        // At very low zoom, skip vertices to keep performance up
        int skip = 1;
        if (zoom <= 3) skip = 4;
        else if (zoom <= 5) skip = 2;
        renderPolygons(driver, mapWidget, data50m, skip);
    }
}

void CoastlineRenderer::renderPolygons(irr::video::IVideoDriver* driver,
                                        const MapWidget& mapWidget,
                                        const CoastlineData& data,
                                        int skipFactor) {
    // Get viewport bounds in lat/lon
    double centerLat = mapWidget.getCenterLat();
    double centerLon = mapWidget.getCenterLon();
    int zoom = mapWidget.getZoom();

    // Estimate viewport extent from widget dimensions
    // Use the screenToLatLon method to get the corners
    // We need the widget bounds — approximate from center and zoom
    // At zoom z, each pixel covers roughly 156543*cos(lat)/2^z metres
    // But we can use TileMath to get precise viewport bounds
    double mPerPx = 156543.03392 * std::cos(centerLat * M_PI / 180.0) / (1 << zoom);
    // Approximate viewport as 2000x1500 pixels if we can't get actual size
    // The MapWidget stores dimensions internally; use its screenToLatLon
    TileMath::LatLon topLeft = mapWidget.screenToLatLon(0, 0);
    TileMath::LatLon bottomRight = mapWidget.screenToLatLon(2000, 1500);

    // Use a slightly expanded bounding box for safety
    double viewMinLat = std::min(topLeft.lat, bottomRight.lat) - 1.0;
    double viewMaxLat = std::max(topLeft.lat, bottomRight.lat) + 1.0;
    double viewMinLon = std::min(topLeft.lon, bottomRight.lon) - 1.0;
    double viewMaxLon = std::max(topLeft.lon, bottomRight.lon) + 1.0;

    irr::video::SColor coastColor(255, 220, 200, 50); // Yellow-ish coastline

    const auto& polygons = data.getPolygons();
    for (const auto& poly : polygons) {
        // Bounding box culling
        if (poly.maxLat < viewMinLat || poly.minLat > viewMaxLat) continue;
        if (poly.maxLon < viewMinLon || poly.minLon > viewMaxLon) continue;

        int vertexCount = static_cast<int>(poly.vertices.size()) / 2;
        if (vertexCount < 2) continue;

        // Draw line segments
        TileMath::PixelPos prevPx = {0, 0};
        bool hasPrev = false;

        for (int v = 0; v < vertexCount; v += skipFactor) {
            float lon = poly.vertices[v * 2];
            float lat = poly.vertices[v * 2 + 1];

            TileMath::PixelPos px = mapWidget.latLonToScreen(lat, lon);

            if (hasPrev) {
                // Skip very long lines (likely crossing the antimeridian or off-screen)
                int dx = px.x - prevPx.x;
                int dy = px.y - prevPx.y;
                if (dx * dx + dy * dy < 4000000) { // ~2000 pixel max segment
                    driver->draw2DLine(
                        irr::core::position2d<irr::s32>(prevPx.x, prevPx.y),
                        irr::core::position2d<irr::s32>(px.x, px.y),
                        coastColor);
                }
            }
            prevPx = px;
            hasPrev = true;
        }

        // Close the polygon: connect last vertex back to first
        if (hasPrev && vertexCount > 2) {
            float lon0 = poly.vertices[0];
            float lat0 = poly.vertices[1];
            TileMath::PixelPos px0 = mapWidget.latLonToScreen(lat0, lon0);
            int dx = px0.x - prevPx.x;
            int dy = px0.y - prevPx.y;
            if (dx * dx + dy * dy < 4000000) {
                driver->draw2DLine(
                    irr::core::position2d<irr::s32>(prevPx.x, prevPx.y),
                    irr::core::position2d<irr::s32>(px0.x, px0.y),
                    coastColor);
            }
        }
    }
}
