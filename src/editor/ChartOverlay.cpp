#include "ChartOverlay.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "../graphics/wicked/imgui/imgui.h"
#include <cmath>
#include <algorithm>
#include <cstdio>

#ifdef WITH_GDAL
#include "../ChartReader.hpp"
#endif

bool ChartOverlay::isGdalAvailable() {
#ifdef WITH_GDAL
    return true;
#else
    return false;
#endif
}

bool ChartOverlay::loadChart(const std::string& chartPath) {
    clear();
    return addChart(chartPath);
}

bool ChartOverlay::addChart(const std::string& chartPath) {
#ifdef WITH_GDAL
    ChartReader reader;
    if (!reader.open(chartPath))
        return false;

    // Extract all features
    auto chartBuoys = reader.extractBuoys();
    auto chartLights = reader.extractLights();
    auto chartDepths = reader.extractDepthAreas();
    auto chartSoundings = reader.extractSoundings();
    auto chartCoastlines = reader.extractCoastlines();
    auto chartLandmarks = reader.extractLandmarks();
    auto chartTSS = reader.extractTSSAreas();

    // Initialize extent if this is the first chart
    if (!chartLoaded) {
        extMinLat = 90; extMaxLat = -90;
        extMinLon = 180; extMaxLon = -180;
    }

    auto updateExtent = [&](double lat, double lon) {
        if (lat < extMinLat) extMinLat = lat;
        if (lat > extMaxLat) extMaxLat = lat;
        if (lon < extMinLon) extMinLon = lon;
        if (lon > extMaxLon) extMaxLon = lon;
    };

    // Depth areas
    for (const auto& da : chartDepths) {
        OverlayDepthArea oda;
        oda.minDepth = da.minDepth;
        oda.maxDepth = da.maxDepth;
        for (const auto& pt : da.boundary) {
            oda.boundary.push_back({(float)pt.latitude, (float)pt.longitude});
            updateExtent(pt.latitude, pt.longitude);
        }
        depthAreas.push_back(std::move(oda));
    }

    // Soundings
    for (const auto& s : chartSoundings) {
        OverlaySounding os;
        os.lat = (float)s.latitude;
        os.lon = (float)s.longitude;
        os.depth = (float)s.depth;
        soundings.push_back(os);
        updateExtent(s.latitude, s.longitude);
    }

    // Coastlines
    for (const auto& seg : chartCoastlines) {
        OverlayCoastline oc;
        for (const auto& pt : seg.points) {
            oc.points.push_back({(float)pt.latitude, (float)pt.longitude});
            updateExtent(pt.latitude, pt.longitude);
        }
        coastlines.push_back(std::move(oc));
    }

    // Buoys
    for (const auto& b : chartBuoys) {
        OverlayBuoy ob;
        ob.lat = (float)b.latitude;
        ob.lon = (float)b.longitude;
        ob.name = b.name;
        ob.layerName = b.layerName;
        if (b.layerName == "BOYLAT") {
            if (b.categoryLateral == 1) { ob.r = 255; ob.g = 0; ob.b = 0; }
            else if (b.categoryLateral == 2) { ob.r = 0; ob.g = 255; ob.b = 0; }
            else { ob.r = 255; ob.g = 200; ob.b = 0; }
        } else if (b.layerName == "BOYCAR") {
            ob.r = 255; ob.g = 255; ob.b = 0;
        } else if (b.layerName == "BOYISD") {
            ob.r = 200; ob.g = 0; ob.b = 0;
        } else if (b.layerName == "BOYSAW") {
            ob.r = 255; ob.g = 0; ob.b = 0;
        } else {
            ob.r = 255; ob.g = 255; ob.b = 0;
        }
        buoys.push_back(std::move(ob));
        updateExtent(b.latitude, b.longitude);
    }

    // Lights
    for (const auto& l : chartLights) {
        OverlayLight ol;
        ol.lat = (float)l.latitude;
        ol.lon = (float)l.longitude;
        ol.range = (float)l.range;
        int r, g, b;
        ChartReader::colourToRGB(l.colour, r, g, b);
        ol.r = (uint8_t)r;
        ol.g = (uint8_t)g;
        ol.b = (uint8_t)b;
        lights.push_back(ol);
        updateExtent(l.latitude, l.longitude);
    }

    // Landmarks
    for (const auto& lm : chartLandmarks) {
        OverlayLandmark olm;
        olm.lat = (float)lm.latitude;
        olm.lon = (float)lm.longitude;
        olm.name = lm.name;
        olm.category = lm.category;
        landmarks.push_back(std::move(olm));
        updateExtent(lm.latitude, lm.longitude);
    }

    // TSS areas
    for (const auto& tss : chartTSS) {
        OverlayTSSArea ot;
        ot.layerName = tss.layerName;
        for (const auto& pt : tss.boundary) {
            ot.boundary.push_back({(float)pt.latitude, (float)pt.longitude});
            updateExtent(pt.latitude, pt.longitude);
        }
        tssAreas.push_back(std::move(ot));
    }

    loadedPath = chartPath;
    loadedPaths.push_back(chartPath);
    chartLoaded = true;
    return true;
#else
    (void)chartPath;
    return false;
#endif
}

void ChartOverlay::clear() {
    depthAreas.clear();
    soundings.clear();
    coastlines.clear();
    buoys.clear();
    lights.clear();
    landmarks.clear();
    tssAreas.clear();
    chartLoaded = false;
    loadedPath.clear();
    loadedPaths.clear();
    extMinLat = extMaxLat = extMinLon = extMaxLon = 0;
}

void ChartOverlay::getExtent(double& minLat, double& maxLat,
                              double& minLon, double& maxLon) const {
    minLat = extMinLat;
    maxLat = extMaxLat;
    minLon = extMinLon;
    maxLon = extMaxLon;
}

// ── Colour for depth areas ──────────────────────────────────────────────────

unsigned int ChartOverlay::depthToColor(double minDepth, double maxDepth) {
    // Shallow water is lighter blue, deep water is darker
    // Negative depth = above water (land) = tan/brown
    double d = (minDepth + maxDepth) * 0.5;

    if (d < 0) {
        // Land / drying area
        return IM_COL32(180, 170, 130, 80); // tan
    } else if (d < 2) {
        return IM_COL32(150, 200, 255, 60); // very shallow - light cyan
    } else if (d < 5) {
        return IM_COL32(120, 180, 240, 60); // shallow
    } else if (d < 10) {
        return IM_COL32(90, 150, 220, 50);
    } else if (d < 20) {
        return IM_COL32(60, 120, 200, 45);
    } else if (d < 50) {
        return IM_COL32(40, 90, 180, 40);
    } else {
        return IM_COL32(20, 60, 150, 35); // deep
    }
}

// ── Rendering ───────────────────────────────────────────────────────────────

void ChartOverlay::render(ImDrawList* drawList, int zoom,
                           OverlayToPixelFn toPixel, void* userData,
                           float mapX, float mapY, float mapW, float mapH) {
    if (!chartLoaded)
        return;

    // Draw in order: depth areas (back) -> TSS -> coastlines -> soundings -> buoys -> lights -> landmarks (front)
    renderDepthAreas(drawList, zoom, toPixel, userData);
    renderTSSAreas(drawList, zoom, toPixel, userData);
    renderCoastlines(drawList, zoom, toPixel, userData);
    if (zoom >= 12)
        renderSoundings(drawList, zoom, toPixel, userData, mapX, mapY, mapW, mapH);
    if (zoom >= 10)
        renderBuoys(drawList, zoom, toPixel, userData, mapX, mapY, mapW, mapH);
    if (zoom >= 10)
        renderLights(drawList, zoom, toPixel, userData, mapX, mapY, mapW, mapH);
    if (zoom >= 12)
        renderLandmarks(drawList, zoom, toPixel, userData, mapX, mapY, mapW, mapH);
}

void ChartOverlay::renderDepthAreas(ImDrawList* drawList, int zoom,
                                     OverlayToPixelFn toPixel, void* userData) {
    if (zoom < 8) return; // Too zoomed out for depth areas

    for (const auto& da : depthAreas) {
        if (da.boundary.size() < 3) continue;

        unsigned int col = depthToColor(da.minDepth, da.maxDepth);

        // Convert boundary to pixel coords
        // ImDrawList can draw convex polygons; for concave we use AddConvexPolyFilled
        // which works reasonably for most depth area shapes at map scale
        std::vector<ImVec2> pts;
        pts.reserve(da.boundary.size());
        for (const auto& p : da.boundary) {
            pts.push_back(toPixel(p.x, p.y, userData)); // x=lat, y=lon
        }

        // Use PathFillConvex for simple fill (works for convex shapes)
        // For complex polygons, use multiple PathLineTo + PathFillConvex
        if (pts.size() >= 3) {
            drawList->AddConvexPolyFilled(pts.data(), (int)pts.size(), col);
        }
    }
}

void ChartOverlay::renderSoundings(ImDrawList* drawList, int zoom,
                                    OverlayToPixelFn toPixel, void* userData,
                                    float mapX, float mapY, float mapW, float mapH) {
    // Only show at zoom >= 12
    for (const auto& s : soundings) {
        ImVec2 pos = toPixel(s.lat, s.lon, userData);
        // Cull off-screen
        if (pos.x < mapX - 20 || pos.x > mapX + mapW + 20 ||
            pos.y < mapY - 10 || pos.y > mapY + mapH + 10)
            continue;

        char label[16];
        if (s.depth < 10)
            snprintf(label, sizeof(label), "%.1f", s.depth);
        else
            snprintf(label, sizeof(label), "%.0f", s.depth);

        // Small text for sounding depths
        ImU32 textCol = (s.depth < 5) ? IM_COL32(50, 150, 255, 200)
                                       : IM_COL32(100, 180, 255, 160);
        ImVec2 textSize = ImGui::CalcTextSize(label);
        drawList->AddText(ImVec2(pos.x - textSize.x * 0.5f, pos.y - textSize.y * 0.5f),
                          textCol, label);
    }
}

void ChartOverlay::renderCoastlines(ImDrawList* drawList, int zoom,
                                     OverlayToPixelFn toPixel, void* userData) {
    float thickness = (zoom >= 12) ? 2.5f : 1.5f;
    ImU32 coastCol = IM_COL32(180, 160, 100, 200); // Sandy/tan colour

    for (const auto& seg : coastlines) {
        if (seg.points.size() < 2) continue;

        ImVec2 prev = toPixel(seg.points[0].x, seg.points[0].y, userData);
        for (size_t i = 1; i < seg.points.size(); i++) {
            ImVec2 cur = toPixel(seg.points[i].x, seg.points[i].y, userData);
            drawList->AddLine(prev, cur, coastCol, thickness);
            prev = cur;
        }
    }
}

void ChartOverlay::renderBuoys(ImDrawList* drawList, int zoom,
                                OverlayToPixelFn toPixel, void* userData,
                                float mapX, float mapY, float mapW, float mapH) {
    for (const auto& b : buoys) {
        ImVec2 pos = toPixel(b.lat, b.lon, userData);
        if (pos.x < mapX - 10 || pos.x > mapX + mapW + 10 ||
            pos.y < mapY - 10 || pos.y > mapY + mapH + 10)
            continue;

        ImU32 col = IM_COL32(b.r, b.g, b.b, 220);
        float s = 5.0f;
        // Diamond shape
        drawList->AddQuadFilled(
            ImVec2(pos.x, pos.y - s), ImVec2(pos.x + s, pos.y),
            ImVec2(pos.x, pos.y + s), ImVec2(pos.x - s, pos.y), col);
        drawList->AddQuad(
            ImVec2(pos.x, pos.y - s), ImVec2(pos.x + s, pos.y),
            ImVec2(pos.x, pos.y + s), ImVec2(pos.x - s, pos.y),
            IM_COL32(0, 0, 0, 200), 1.0f);

        if (zoom >= 14 && !b.name.empty()) {
            drawList->AddText(ImVec2(pos.x + s + 2, pos.y - 6),
                              IM_COL32(b.r, b.g, b.b, 200), b.name.c_str());
        }
    }
}

void ChartOverlay::renderLights(ImDrawList* drawList, int zoom,
                                 OverlayToPixelFn toPixel, void* userData,
                                 float mapX, float mapY, float mapW, float mapH) {
    for (const auto& l : lights) {
        ImVec2 pos = toPixel(l.lat, l.lon, userData);
        if (pos.x < mapX - 15 || pos.x > mapX + mapW + 15 ||
            pos.y < mapY - 15 || pos.y > mapY + mapH + 15)
            continue;

        ImU32 col = IM_COL32(l.r, l.g, l.b, 220);
        ImU32 glowCol = IM_COL32(l.r, l.g, l.b, 60);
        float r = 3.5f;
        float glowR = r + std::min(l.range * 0.5f, 14.0f);
        drawList->AddCircleFilled(pos, glowR, glowCol, 12);
        drawList->AddCircleFilled(pos, r, col, 8);

        if (zoom >= 14) {
            char label[32];
            snprintf(label, sizeof(label), "%.0fNM", l.range);
            drawList->AddText(ImVec2(pos.x + r + 2, pos.y - 6),
                              IM_COL32(l.r, l.g, l.b, 180), label);
        }
    }
}

void ChartOverlay::renderLandmarks(ImDrawList* drawList, int zoom,
                                    OverlayToPixelFn toPixel, void* userData,
                                    float mapX, float mapY, float mapW, float mapH) {
    for (const auto& lm : landmarks) {
        ImVec2 pos = toPixel(lm.lat, lm.lon, userData);
        if (pos.x < mapX - 10 || pos.x > mapX + mapW + 10 ||
            pos.y < mapY - 10 || pos.y > mapY + mapH + 10)
            continue;

        // Small square icon for landmarks
        float s = 3.0f;
        ImU32 col = IM_COL32(160, 120, 80, 200);
        drawList->AddRectFilled(ImVec2(pos.x - s, pos.y - s),
                                ImVec2(pos.x + s, pos.y + s), col);
        drawList->AddRect(ImVec2(pos.x - s, pos.y - s),
                          ImVec2(pos.x + s, pos.y + s),
                          IM_COL32(0, 0, 0, 180), 0.0f, 0, 1.0f);

        if (zoom >= 14 && !lm.name.empty()) {
            drawList->AddText(ImVec2(pos.x + s + 2, pos.y - 6),
                              IM_COL32(180, 160, 120, 200), lm.name.c_str());
        }
    }
}

void ChartOverlay::renderTSSAreas(ImDrawList* drawList, int zoom,
                                   OverlayToPixelFn toPixel, void* userData) {
    if (zoom < 6) return;

    // Magenta fill (standard chart colour for TSS)
    ImU32 fillCol = IM_COL32(200, 0, 200, 40);
    ImU32 borderCol = IM_COL32(200, 0, 200, 120);

    for (const auto& tss : tssAreas) {
        if (tss.boundary.size() < 3) continue;

        std::vector<ImVec2> pts;
        pts.reserve(tss.boundary.size());
        for (const auto& p : tss.boundary) {
            pts.push_back(toPixel(p.x, p.y, userData));
        }

        // Filled polygon
        if (pts.size() >= 3) {
            drawList->AddConvexPolyFilled(pts.data(), (int)pts.size(), fillCol);
        }

        // Border outline
        for (size_t i = 0; i < pts.size(); i++) {
            size_t j = (i + 1) % pts.size();
            drawList->AddLine(pts[i], pts[j], borderCol, 1.5f);
        }
    }
}
