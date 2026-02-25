/*   Bridge Command 5.0 Ship Simulator
     Copyright (C) 2024 James Packer

     This program is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License version 2 as
     published by the Free Software Foundation */

#ifdef WITH_WICKED_ENGINE

#include "EcdisDisplay.hpp"
#include "../graphics/wicked/imgui/imgui.h"
#include "../editor/TileDownloader.hpp"
#include "../editor/TileMath.hpp"
#include "../libs/stb/stb_image.h"

#include "WickedEngine.h"

#include <cstring>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static constexpr uint32_t COL(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
}

namespace bc { namespace gui {

// ============================================================================
// GPU tile cache
// ============================================================================

struct EcdisDisplay::GPUTileCacheImpl {
    struct Entry {
        wi::graphics::Texture texture;
        bool valid = false;
    };
    std::unordered_map<std::string, Entry> cache;
    std::vector<std::string> lru;

    void clear() { cache.clear(); lru.clear(); }

    void evict(size_t maxSize) {
        while (cache.size() >= maxSize && !lru.empty()) {
            cache.erase(lru.front());
            lru.erase(lru.begin());
        }
    }

    void touchLRU(const std::string& key) {
        auto it = std::find(lru.begin(), lru.end(), key);
        if (it != lru.end()) lru.erase(it);
        lru.push_back(key);
    }
};

// ============================================================================
// Construction / Init
// ============================================================================

EcdisDisplay::EcdisDisplay() {
    tileCache_ = std::make_unique<GPUTileCacheImpl>();
}
EcdisDisplay::~EcdisDisplay() {}

void EcdisDisplay::init(const std::string& cacheDir) {
    if (initialized_) return;
    osmDownloader_ = std::make_unique<TileDownloader>(
        "https://tile.openstreetmap.org/{z}/{x}/{y}.png", cacheDir + "osm/");
    osmDownloader_->setUserAgent("BridgeCommand/6.0 (ecdis)");
    seamarkDownloader_ = std::make_unique<TileDownloader>(
        "https://tiles.openseamap.org/seamark/{z}/{x}/{y}.png", cacheDir + "seamark/");
    seamarkDownloader_->setUserAgent("BridgeCommand/6.0 (ecdis)");
    initialized_ = true;
}

void EcdisDisplay::setOwnShipData(const OwnShipData& data) { ownShip_ = data; }
void EcdisDisplay::setAISTargets(const std::vector<AISTarget>& targets) { aisTargets_ = targets; }

// ============================================================================
// Coordinate transforms
// ============================================================================

EcdisDisplay::ScreenPos EcdisDisplay::latLonToScreen(double lat, double lon) const {
    float dx = (float)(TileMath::lonToPixelX(lon, zoom_) - TileMath::lonToPixelX(centerLon_, zoom_));
    float dy = (float)(TileMath::latToPixelY(lat, zoom_) - TileMath::latToPixelY(centerLat_, zoom_));
    return { chartX_ + chartW_ * 0.5f + dx, chartY_ + chartH_ * 0.5f + dy };
}

EcdisDisplay::LatLonPos EcdisDisplay::screenToLatLon(float sx, float sy) const {
    int px = (int)std::round(sx - chartX_ - chartW_ * 0.5f);
    int py = (int)std::round(sy - chartY_ - chartH_ * 0.5f);
    auto ll = TileMath::pixelToLatLon(centerLat_, centerLon_, zoom_, px, py);
    return { ll.lat, ll.lon };
}

float EcdisDisplay::metersPerPixel() const {
    return (float)(40075017.0 * std::cos(centerLat_ * M_PI / 180.0) / (256.0 * (1 << zoom_)));
}

float EcdisDisplay::nmToPixels(float nm) const {
    float mpp = metersPerPixel();
    return mpp > 0 ? nm * 1852.0f / mpp : 0;
}

uint32_t EcdisDisplay::tileTint() const {
    switch (paletteIndex_) {
        case 0: return COL(255, 255, 255);
        case 1: return COL(140, 140, 160);
        case 2: return COL(50, 50, 70);
        default: return COL(255, 255, 255);
    }
}

void EcdisDisplay::calcBearingDistance(double lat1, double lon1,
                                        double lat2, double lon2,
                                        float& bearingDeg, float& distanceNm) {
    double dLat = (lat2 - lat1) * M_PI / 180.0;
    double dLon = (lon2 - lon1) * M_PI / 180.0;
    double r1 = lat1 * M_PI / 180.0, r2 = lat2 * M_PI / 180.0;
    double a = std::sin(dLat/2)*std::sin(dLat/2) +
               std::cos(r1)*std::cos(r2)*std::sin(dLon/2)*std::sin(dLon/2);
    distanceNm = (float)(2 * std::atan2(std::sqrt(a), std::sqrt(1-a)) * 3440.065);
    double y = std::sin(dLon) * std::cos(r2);
    double x = std::cos(r1)*std::sin(r2) - std::sin(r1)*std::cos(r2)*std::cos(dLon);
    bearingDeg = (float)(std::atan2(y, x) * 180.0 / M_PI);
    if (bearingDeg < 0) bearingDeg += 360.0f;
}

// ============================================================================
// Tile texture management
// ============================================================================

ImTextureID EcdisDisplay::getOrCreateTileTexture(int z, int x, int y) {
    char key[64];
    snprintf(key, sizeof(key), "%d/%d/%d", z, x, y);
    std::string keyStr(key);

    auto it = tileCache_->cache.find(keyStr);
    if (it != tileCache_->cache.end() && it->second.valid) {
        tileCache_->touchLRU(keyStr);
        return (ImTextureID)&it->second.texture;
    }

    if (!osmDownloader_) return nullptr;
    std::vector<uint8_t> tileData = osmDownloader_->getTile(z, x, y);
    if (tileData.empty()) return nullptr;

    int tw = 0, th = 0, tc = 0;
    unsigned char* decoded = stbi_load_from_memory(tileData.data(), (int)tileData.size(), &tw, &th, &tc, 4);
    if (!decoded) return nullptr;

    if (seamarkDownloader_) {
        std::vector<uint8_t> sd = seamarkDownloader_->getTile(z, x, y);
        if (!sd.empty()) {
            int sw, sh, sc2;
            unsigned char* sm = stbi_load_from_memory(sd.data(), (int)sd.size(), &sw, &sh, &sc2, 4);
            if (sm && sw == tw && sh == th) {
                for (int i = 0; i < tw * th; i++) {
                    uint8_t a = sm[i*4+3];
                    if (a == 0) continue;
                    float af = a / 255.0f, inv = 1.0f - af;
                    decoded[i*4+0] = (uint8_t)(sm[i*4+0]*af + decoded[i*4+0]*inv);
                    decoded[i*4+1] = (uint8_t)(sm[i*4+1]*af + decoded[i*4+1]*inv);
                    decoded[i*4+2] = (uint8_t)(sm[i*4+2]*af + decoded[i*4+2]*inv);
                    decoded[i*4+3] = 255;
                }
            }
            if (sm) stbi_image_free(sm);
        }
    }

    evictOldTiles();

    auto& entry = tileCache_->cache[keyStr];
    entry.valid = false;
    wi::graphics::TextureDesc desc;
    desc.width = tw; desc.height = th;
    desc.format = wi::graphics::Format::R8G8B8A8_UNORM;
    desc.bind_flags = wi::graphics::BindFlag::SHADER_RESOURCE;
    desc.mip_levels = 1; desc.array_size = 1;
    wi::graphics::SubresourceData sd;
    sd.data_ptr = decoded; sd.row_pitch = tw * 4; sd.slice_pitch = sd.row_pitch * th;
    wi::graphics::GetDevice()->CreateTexture(&desc, &sd, &entry.texture);
    stbi_image_free(decoded);

    if (!entry.texture.IsValid()) { tileCache_->cache.erase(keyStr); return nullptr; }
    entry.valid = true;
    tileCache_->lru.push_back(keyStr);
    return (ImTextureID)&entry.texture;
}

void EcdisDisplay::evictOldTiles() { tileCache_->evict(MAX_GPU_TILES); }
void EcdisDisplay::clearTileCache() { tileCache_->clear(); tileCacheZoom_ = -1; }

void EcdisDisplay::setZoom(int z) {
    z = std::max(3, std::min(18, z));
    if (z != zoom_) {
        zoom_ = z;
        if (tileCacheZoom_ != zoom_) {
            clearTileCache();
            tileCacheZoom_ = zoom_;
        }
    }
}

// ============================================================================
// Main render
// ============================================================================

bool EcdisDisplay::render(int screenWidth, int screenHeight) {
    if (!initialized_) return true;
    bool open = true;
    float w = (float)screenWidth, h = (float)screenHeight;
    float statusBarH = 28.0f;

    if (centerOnShip_) { centerLat_ = ownShip_.lat; centerLon_ = ownShip_.lon; }
    closeRequested_ = false;

    // Sample track history
    if (ownShip_.simulationTime - lastTrackSampleTime_ >= TRACK_SAMPLE_INTERVAL) {
        trackHistory_.push_back({ownShip_.lat, ownShip_.lon, ownShip_.heading, ownShip_.simulationTime});
        if (trackHistory_.size() > MAX_TRACK_POINTS)
            trackHistory_.erase(trackHistory_.begin());
        lastTrackSampleTime_ = ownShip_.simulationTime;
    }

    chartX_ = 0; chartY_ = 0;
    chartW_ = w; chartH_ = h - statusBarH;

    // Keyboard shortcuts
    if (!ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_KeypadAdd) || ImGui::IsKeyPressed(ImGuiKey_Equal)) setZoom(zoom_ + 1);
        if (ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract) || ImGui::IsKeyPressed(ImGuiKey_Minus)) setZoom(zoom_ - 1);
        if (ImGui::IsKeyPressed(ImGuiKey_F5)) paletteIndex_ = (paletteIndex_ + 1) % 3;
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
            if (selectedWaypointIdx_ >= 0 && selectedWaypointIdx_ < (int)routeWaypoints_.size()) {
                routeWaypoints_.erase(routeWaypoints_.begin() + selectedWaypointIdx_);
                selectedWaypointIdx_ = -1;
            } else if (selectedMarkerIdx_ >= 0 && selectedMarkerIdx_ < (int)chartMarkers_.size()) {
                chartMarkers_.erase(chartMarkers_.begin() + selectedMarkerIdx_);
                selectedMarkerIdx_ = -1;
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            if (interactionMode_ != InteractionMode::Navigate)
                interactionMode_ = InteractionMode::Navigate;
        }
    }

    // Full-screen window (zero padding so InvisibleButton covers full chart area)
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    uint32_t bgCol = (paletteIndex_ == 2) ? COL(4, 4, 16) :
                     (paletteIndex_ == 1) ? COL(20, 20, 30) : COL(200, 210, 220);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(bgCol));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##EcdisFullscreen", nullptr, flags);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Invisible button for chart area drag handling
    ImGui::SetCursorPos(ImVec2(chartX_, chartY_));
    ImGui::InvisibleButton("##ChartArea", ImVec2(chartW_, chartH_));
    bool chartActive = ImGui::IsItemActive();

    // Raw mouse position check (like RadarDisplay's overPPI approach -- more
    // reliable than InvisibleButton hover which can be blocked by ImGui window focus)
    ImGuiIO& io = ImGui::GetIO();
    float mx = io.MousePos.x, my = io.MousePos.y;
    bool mouseInChart = (mx >= chartX_ && mx < chartX_ + chartW_ &&
                         my >= chartY_ && my < chartY_ + chartH_);

    // Clip to chart area and render layers
    dl->PushClipRect(ImVec2(chartX_, chartY_), ImVec2(chartX_ + chartW_, chartY_ + chartH_), true);
    renderTiles(dl);
    renderTrackHistory(dl);
    renderRoute(dl);
    renderAISTargets(dl);
    renderOwnShip(dl);
    if (showDREP_) renderDREP(dl);
    renderMeasurements(dl);
    renderMarkers(dl);

    // Cursor crosshair (uses raw position check for reliability)
    if (mouseInChart) {
        bool addMode = (interactionMode_ == InteractionMode::AddWaypoint);
        bool msrMode = (interactionMode_ == InteractionMode::MeasureBearingDistance);
        bool mrkMode = (interactionMode_ == InteractionMode::PlaceMarker);
        uint32_t cc = addMode ? COL(255, 0, 255) :
                      msrMode ? COL(255, 255, 0) :
                      mrkMode ? COL(0, 200, 255) :
                      (paletteIndex_ == 2) ? COL(200, 60, 60) : COL(255, 0, 0);
        float gap = (addMode || msrMode || mrkMode) ? 5.0f : 3.0f;
        float len = (addMode || msrMode || mrkMode) ? 15.0f : 10.0f;
        dl->AddLine(ImVec2(mx - len, my), ImVec2(mx - gap, my), cc, 1.0f);
        dl->AddLine(ImVec2(mx + gap, my), ImVec2(mx + len, my), cc, 1.0f);
        dl->AddLine(ImVec2(mx, my - len), ImVec2(mx, my - gap), cc, 1.0f);
        dl->AddLine(ImVec2(mx, my + gap), ImVec2(mx, my + len), cc, 1.0f);
        if (addMode) {
            char wpBuf[16];
            snprintf(wpBuf, sizeof(wpBuf), "WP%02d", (int)routeWaypoints_.size() + 1);
            dl->AddText(ImVec2(mx + 12, my - 14), COL(255, 0, 255), wpBuf);
        }
        // Measurement rubber-band line from first point to cursor
        if (msrMode && activeMeasureStep_ == 1) {
            ScreenPos p1 = latLonToScreen(measureLat1_, measureLon1_);
            dl->AddLine(ImVec2(p1.x, p1.y), ImVec2(mx, my), COL(255, 255, 0, 180), 1.0f);
            auto cll = screenToLatLon(mx, my);
            float brg, dist;
            calcBearingDistance(measureLat1_, measureLon1_, cll.lat, cll.lon, brg, dist);
            char mb[48]; snprintf(mb, sizeof(mb), "%03.0f / %.2f nm", brg, dist);
            dl->AddText(ImVec2(mx + 12, my - 14), COL(255, 255, 0), mb);
        }
        if (mrkMode) {
            char mb[16]; snprintf(mb, sizeof(mb), "M%02d", (int)chartMarkers_.size() + 1);
            dl->AddText(ImVec2(mx + 12, my - 14), COL(0, 200, 255), mb);
        }
    }
    dl->PopClipRect();

    // Compass rose and scale bar
    renderCompassRose(dl, w - 60, 60, 45);
    renderScaleBar(dl, 60, chartH_ - 20);

    // Render floating overlays BEFORE processing interactions so we can
    // track whether any floating window is under the cursor.
    floatingWindowHovered_ = false;
    if (showDataBox_) renderDataBox(w);
    renderToolbar(h);
    if (selectedAISIdx_ >= 0 && selectedAISIdx_ < (int)aisTargets_.size()) {
        renderAISInfoPopup();
    }
    renderStatusBar(0, h - statusBarH, w, statusBarH);

    // Chart interactions -- use raw position check, skip if over a floating window
    bool canClickChart = mouseInChart && !floatingWindowHovered_;

    if (canClickChart || chartActive) {
        // Zoom with mouse wheel
        if (canClickChart) {
            float wheel = io.MouseWheel;
            if (wheel != 0) {
                int newZ = zoom_ + (wheel > 0 ? 1 : -1);
                if (!centerOnShip_) {
                    auto cur = screenToLatLon(mx, my);
                    setZoom(newZ);
                    auto newPos = latLonToScreen(cur.lat, cur.lon);
                    auto cll = screenToLatLon(
                        chartX_ + chartW_*0.5f - (newPos.x - mx),
                        chartY_ + chartH_*0.5f - (newPos.y - my));
                    centerLat_ = cll.lat; centerLon_ = cll.lon;
                } else {
                    setZoom(newZ);
                }
            }
        }

        if (interactionMode_ == InteractionMode::AddWaypoint) {
            // Left click: add waypoint
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && canClickChart) {
                auto ll = screenToLatLon(mx, my);
                routeWaypoints_.push_back({ll.lat, ll.lon});
            }
            // Right click: finish route input
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                interactionMode_ = InteractionMode::Navigate;
            }
        } else if (interactionMode_ == InteractionMode::MeasureBearingDistance) {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && canClickChart) {
                auto ll = screenToLatLon(mx, my);
                if (activeMeasureStep_ == 0) {
                    measureLat1_ = ll.lat; measureLon1_ = ll.lon;
                    activeMeasureStep_ = 1;
                } else {
                    measurements_.push_back({measureLat1_, measureLon1_, ll.lat, ll.lon});
                    activeMeasureStep_ = 0;
                }
            }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                activeMeasureStep_ = 0;
                interactionMode_ = InteractionMode::Navigate;
            }
        } else if (interactionMode_ == InteractionMode::PlaceMarker) {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && canClickChart) {
                auto ll = screenToLatLon(mx, my);
                char lbl[16]; snprintf(lbl, sizeof(lbl), "M%02d", (int)chartMarkers_.size() + 1);
                chartMarkers_.push_back({ll.lat, ll.lon, std::string(lbl)});
            }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                interactionMode_ = InteractionMode::Navigate;
            }
        } else {
            // Navigate mode: pan with left drag
            if (chartActive && io.MouseDown[0]) {
                if (!isDragging_) {
                    isDragging_ = true;
                    dragStartLat_ = centerLat_;
                    dragStartLon_ = centerLon_;
                    centerOnShip_ = false;
                }
                ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 2.0f);
                if (delta.x != 0 || delta.y != 0) {
                    auto nc = TileMath::pixelToLatLon(dragStartLat_, dragStartLon_, zoom_,
                                                       (int)(-delta.x), (int)(-delta.y));
                    centerLat_ = nc.lat; centerLon_ = nc.lon;
                }
            } else {
                if (isDragging_) {
                    isDragging_ = false;
                    ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
                }
            }

            // Right click on chart: check for AIS target / waypoint / marker selection
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && canClickChart) {
                selectedAISIdx_ = -1;
                selectedWaypointIdx_ = -1;
                selectedMarkerIdx_ = -1;
                float bestDist = 20.0f;
                for (int i = 0; i < (int)aisTargets_.size(); i++) {
                    ScreenPos sp = latLonToScreen(aisTargets_[i].lat, aisTargets_[i].lon);
                    float d = std::sqrt((sp.x-mx)*(sp.x-mx) + (sp.y-my)*(sp.y-my));
                    if (d < bestDist) { bestDist = d; selectedAISIdx_ = i; }
                }
                bestDist = 15.0f;
                for (int i = 0; i < (int)routeWaypoints_.size(); i++) {
                    ScreenPos sp = latLonToScreen(routeWaypoints_[i].lat, routeWaypoints_[i].lon);
                    float d = std::sqrt((sp.x-mx)*(sp.x-mx) + (sp.y-my)*(sp.y-my));
                    if (d < bestDist) { bestDist = d; selectedWaypointIdx_ = i; }
                }
                bestDist = 15.0f;
                for (int i = 0; i < (int)chartMarkers_.size(); i++) {
                    ScreenPos sp = latLonToScreen(chartMarkers_[i].lat, chartMarkers_[i].lon);
                    float d = std::sqrt((sp.x-mx)*(sp.x-mx) + (sp.y-my)*(sp.y-my));
                    if (d < bestDist) { bestDist = d; selectedMarkerIdx_ = i; }
                }
            }

            // Double-click to center
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && canClickChart) {
                auto ll = screenToLatLon(mx, my);
                centerLat_ = ll.lat; centerLon_ = ll.lon;
                centerOnShip_ = false;
            }
        }
    } else {
        if (isDragging_) {
            isDragging_ = false;
            ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
        }
    }

    ImGui::End();
    if (closeRequested_) open = false;
    return open;
}

// ============================================================================
// Tile rendering
// ============================================================================

void EcdisDisplay::renderTiles(void* dlp) {
    ImDrawList* dl = (ImDrawList*)dlp;
    if (!osmDownloader_) {
        dl->AddText(ImVec2(chartX_ + 20, chartY_ + 20), COL(255, 0, 0), "ERROR: No tile downloader");
        return;
    }
    const int TS = 256;
    uint32_t tint = tileTint();

    double cpx = TileMath::lonToPixelX(centerLon_, zoom_, TS);
    double cpy = TileMath::latToPixelY(centerLat_, zoom_, TS);
    float scx = chartX_ + chartW_ * 0.5f;
    float scy = chartY_ + chartH_ * 0.5f;

    int txMin = (int)std::floor((cpx - scx) / TS);
    int txMax = (int)std::floor((cpx + (chartW_ - scx)) / TS);
    int tyMin = (int)std::floor((cpy - scy) / TS);
    int tyMax = (int)std::floor((cpy + (chartH_ - scy)) / TS);
    int maxT = (1 << zoom_) - 1;

    for (int ty = tyMin; ty <= tyMax; ty++) {
        for (int tx = txMin; tx <= txMax; tx++) {
            int wx = ((tx % (maxT+1)) + (maxT+1)) % (maxT+1);
            int wy = std::max(0, std::min(maxT, ty));
            ImTextureID tid = getOrCreateTileTexture(zoom_, wx, wy);
            if (!tid) continue;
            float dx = scx + (float)(tx * TS - cpx);
            float dy = scy + (float)(ty * TS - cpy);
            dl->AddImage(tid, ImVec2(dx, dy), ImVec2(dx + TS, dy + TS),
                         ImVec2(0,0), ImVec2(1,1), tint);
        }
    }
}

// ============================================================================
// Track history (own ship trail)
// ============================================================================

void EcdisDisplay::renderTrackHistory(void* dlp) {
    ImDrawList* dl = (ImDrawList*)dlp;
    if (trackHistory_.size() < 2) return;

    bool night = (paletteIndex_ == 2);
    uint32_t lineCol = night ? COL(100, 100, 180, 180) : COL(0, 0, 180, 160);
    uint32_t dotCol  = night ? COL(120, 120, 200) : COL(0, 0, 200);
    uint32_t timeCol = night ? COL(140, 140, 180, 200) : COL(40, 40, 120, 220);

    ScreenPos prev = latLonToScreen(trackHistory_[0].lat, trackHistory_[0].lon);
    for (size_t i = 1; i < trackHistory_.size(); i++) {
        ScreenPos cur = latLonToScreen(trackHistory_[i].lat, trackHistory_[i].lon);
        dl->AddLine(ImVec2(prev.x, prev.y), ImVec2(cur.x, cur.y), lineCol, 1.5f);

        // Dot every ~1 minute (20 samples at 3s interval)
        if (i % 20 == 0) {
            dl->AddCircleFilled(ImVec2(cur.x, cur.y), 2.5f, dotCol);
        }
        // Heading tick every ~6 minutes (120 samples)
        if (i % 120 == 0) {
            float hr = trackHistory_[i].heading * (float)M_PI / 180.0f;
            float s = std::sin(hr), c = std::cos(hr);
            dl->AddLine(ImVec2(cur.x - 5*c, cur.y - 5*s),
                        ImVec2(cur.x + 5*c, cur.y + 5*s), dotCol, 1.0f);
        }
        // Timestamp label every ~5 minutes (100 samples at 3s interval = 300s)
        if (i % 100 == 0 && i > 0) {
            float t = trackHistory_[i].time;
            int totalMin = (int)(t / 60.0f);
            int hh = totalMin / 60;
            int mm = totalMin % 60;
            char timeBuf[8];
            snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", hh, mm);
            dl->AddText(ImVec2(cur.x + 4, cur.y - 6), timeCol, timeBuf);
        }
        prev = cur;
    }

    // Line from last track point to current position
    ScreenPos sp = latLonToScreen(ownShip_.lat, ownShip_.lon);
    dl->AddLine(ImVec2(prev.x, prev.y), ImVec2(sp.x, sp.y), lineCol, 1.5f);
}

// ============================================================================
// Route rendering
// ============================================================================

void EcdisDisplay::renderRoute(void* dlp) {
    ImDrawList* dl = (ImDrawList*)dlp;
    if (routeWaypoints_.empty()) return;

    bool night = (paletteIndex_ == 2);
    uint32_t legCol  = night ? COL(200, 0, 200) : COL(180, 0, 180);       // magenta
    uint32_t wpCol   = night ? COL(255, 100, 255) : COL(200, 0, 200);
    uint32_t selCol  = COL(255, 255, 0);                                     // yellow for selected
    uint32_t textCol = night ? COL(220, 100, 220) : COL(160, 0, 160);

    // Draw leg lines
    for (size_t i = 0; i + 1 < routeWaypoints_.size(); i++) {
        ScreenPos a = latLonToScreen(routeWaypoints_[i].lat, routeWaypoints_[i].lon);
        ScreenPos b = latLonToScreen(routeWaypoints_[i+1].lat, routeWaypoints_[i+1].lon);
        dl->AddLine(ImVec2(a.x, a.y), ImVec2(b.x, b.y), legCol, 2.0f);

        // Bearing and distance label at midpoint
        float brg, dist;
        calcBearingDistance(routeWaypoints_[i].lat, routeWaypoints_[i].lon,
                           routeWaypoints_[i+1].lat, routeWaypoints_[i+1].lon, brg, dist);
        float midX = (a.x + b.x) * 0.5f, midY = (a.y + b.y) * 0.5f;
        char legBuf[48];
        snprintf(legBuf, sizeof(legBuf), "%03.0f / %.1f nm", brg, dist);
        dl->AddText(ImVec2(midX + 5, midY - 14), textCol, legBuf);
    }

    // Draw waypoint markers
    for (size_t i = 0; i < routeWaypoints_.size(); i++) {
        ScreenPos p = latLonToScreen(routeWaypoints_[i].lat, routeWaypoints_[i].lon);
        bool selected = ((int)i == selectedWaypointIdx_);
        uint32_t col = selected ? selCol : wpCol;
        float r = selected ? 7.0f : 5.0f;

        dl->AddCircle(ImVec2(p.x, p.y), r, col, 12, 2.0f);
        dl->AddLine(ImVec2(p.x - r, p.y), ImVec2(p.x + r, p.y), col, 1.0f);
        dl->AddLine(ImVec2(p.x, p.y - r), ImVec2(p.x, p.y + r), col, 1.0f);

        char wpName[16];
        snprintf(wpName, sizeof(wpName), "WP%02d", (int)i + 1);
        dl->AddText(ImVec2(p.x + r + 3, p.y - 7), col, wpName);
    }
}

// ============================================================================
// Own ship
// ============================================================================

void EcdisDisplay::renderOwnShip(void* dlp) {
    ImDrawList* dl = (ImDrawList*)dlp;
    ScreenPos p = latLonToScreen(ownShip_.lat, ownShip_.lon);
    bool night = (paletteIndex_ == 2);
    uint32_t shipCol = night ? COL(255, 128, 0) : COL(200, 0, 0);
    uint32_t hdgCol  = night ? COL(255, 255, 0, 180) : COL(200, 0, 0, 180);
    uint32_t cogCol  = night ? COL(255, 200, 0, 130) : COL(150, 0, 0, 130);

    float headRad = ownShip_.heading * (float)M_PI / 180.0f;
    float s = std::sin(headRad), c = std::cos(headRad);

    // Boat outline
    ImVec2 bow(p.x + 12*s, p.y - 12*c);
    ImVec2 portQ(p.x + 4*(-c - s*0.5f), p.y + 4*(-s + c*0.5f));
    ImVec2 stern(p.x - 5*s, p.y + 5*c);
    ImVec2 stbdQ(p.x + 4*(c - s*0.5f), p.y + 4*(s + c*0.5f));
    dl->AddQuadFilled(bow, portQ, stern, stbdQ, shipCol);
    dl->AddQuad(bow, portQ, stern, stbdQ, night ? COL(255, 200, 0) : COL(0, 0, 0), 1.5f);

    // Heading line
    float len = std::max(chartW_, chartH_);
    dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + len*s, p.y - len*c), hdgCol, 1.0f);

    // COG/SOG vector (6-minute)
    if (ownShip_.sog > 0.3f) {
        float cogR = ownShip_.cog * (float)M_PI / 180.0f;
        float px = nmToPixels(ownShip_.sog * 0.1f);
        if (px > 5.0f) {
            float vx = p.x + px * std::sin(cogR), vy = p.y - px * std::cos(cogR);
            dl->AddLine(ImVec2(p.x, p.y), ImVec2(vx, vy), cogCol, 2.0f);
            dl->AddCircleFilled(ImVec2(vx, vy), 3, cogCol);
        }
    }
}

// ============================================================================
// AIS targets
// ============================================================================

void EcdisDisplay::renderAISTargets(void* dlp) {
    ImDrawList* dl = (ImDrawList*)dlp;
    bool night = (paletteIndex_ == 2);
    uint32_t tgtCol = night ? COL(0, 192, 0) : COL(0, 128, 0);
    uint32_t vecCol = night ? COL(0, 140, 0) : COL(0, 100, 0);
    uint32_t txtCol = night ? COL(0, 160, 0) : COL(0, 100, 0);
    uint32_t selCol = COL(255, 255, 0);

    for (int idx = 0; idx < (int)aisTargets_.size(); idx++) {
        const auto& t = aisTargets_[idx];
        ScreenPos p = latLonToScreen(t.lat, t.lon);
        if (p.x < chartX_ - 50 || p.x > chartX_ + chartW_ + 50 ||
            p.y < chartY_ - 50 || p.y > chartY_ + chartH_ + 50) continue;

        bool selected = (idx == selectedAISIdx_);
        uint32_t col = selected ? selCol : tgtCol;

        float hr = t.heading * (float)M_PI / 180.0f;
        float s = std::sin(hr), c = std::cos(hr);
        ImVec2 tip(p.x + 10*s, p.y - 10*c);
        ImVec2 lt(p.x + 5*(-c - s), p.y + 5*(-s + c));
        ImVec2 rt(p.x + 5*(c - s), p.y + 5*(s + c));
        dl->AddTriangleFilled(tip, lt, rt, selected ? COL(255, 255, 0, 40) : COL(0, 0, 0, 0));
        dl->AddTriangle(tip, lt, rt, col, selected ? 2.5f : 1.5f);

        if (t.speed > 0.5f) {
            float vp = nmToPixels(t.speed * 0.1f);
            if (vp > 3) dl->AddLine(ImVec2(p.x, p.y),
                ImVec2(p.x + vp*s, p.y - vp*c), vecCol, 1.0f);
        }

        char buf[16]; snprintf(buf, sizeof(buf), "%d", t.id);
        dl->AddText(ImVec2(p.x + 13, p.y - 6), selected ? selCol : txtCol, buf);
    }
}

// ============================================================================
// AIS info popup (shown when target is right-click selected)
// ============================================================================

void EcdisDisplay::renderAISInfoPopup() {
    if (selectedAISIdx_ < 0 || selectedAISIdx_ >= (int)aisTargets_.size()) return;
    const auto& t = aisTargets_[selectedAISIdx_];
    ScreenPos sp = latLonToScreen(t.lat, t.lon);

    bool night = (paletteIndex_ == 2);
    ImVec4 bg = night ? ImVec4(0.02f, 0.05f, 0.02f, 0.9f) : ImVec4(1, 1, 0.95f, 0.95f);
    ImVec4 tc = night ? ImVec4(0, 0.8f, 0, 1) : ImVec4(0, 0.3f, 0, 1);

    // Position popup near the target
    float popX = std::min(sp.x + 20, chartX_ + chartW_ - 180);
    float popY = std::max(sp.y - 60, chartY_ + 5);
    ImGui::SetNextWindowPos(ImVec2(popX, popY), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, bg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);

    ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

    ImGui::Begin("##AISInfo", nullptr, wf);
    if (ImGui::IsWindowHovered()) floatingWindowHovered_ = true;

    // Name and MMSI
    if (!t.name.empty()) {
        ImGui::TextColored(tc, "%s", t.name.c_str());
    } else {
        ImGui::TextColored(tc, "TARGET %d", t.id);
    }
    if (t.mmsi > 0) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(tc.x*0.7f, tc.y*0.7f, tc.z*0.7f, 1), "(%u)", t.mmsi);
    }
    ImGui::Separator();

    char ns = t.lat >= 0 ? 'N' : 'S', ew = t.lon >= 0 ? 'E' : 'W';
    double aLat = std::fabs(t.lat), aLon = std::fabs(t.lon);
    ImGui::TextColored(tc, "%02d %05.2f'%c %03d %05.2f'%c",
        (int)aLat, (aLat-(int)aLat)*60, ns, (int)aLon, (aLon-(int)aLon)*60, ew);
    ImGui::TextColored(tc, "HDG %05.1f  SPD %.1f kn", t.heading, t.speed);

    float brg, rng;
    calcBearingDistance(ownShip_.lat, ownShip_.lon, t.lat, t.lon, brg, rng);
    ImGui::TextColored(tc, "BRG %05.1f  RNG %.2f nm", brg, rng);

    if (t.length > 0 || t.breadth > 0)
        ImGui::TextColored(tc, "L %.0f m  B %.0f m", t.length, t.breadth);

    // CPA/TCPA (linear extrapolation)
    {
        double toRad = M_PI / 180.0;
        double nmToDeg = 1.0 / 60.0;
        double oSpd = ownShip_.sog; // knots
        double oHdg = ownShip_.cog * toRad;
        double tSpd = t.speed;
        double tHdg = t.heading * toRad;
        double dLat = (t.lat - ownShip_.lat) * 60.0; // nm (approx)
        double dLon = (t.lon - ownShip_.lon) * 60.0 * std::cos(ownShip_.lat * toRad);
        double dvx = tSpd * std::sin(tHdg) - oSpd * std::sin(oHdg); // relative vel nm/h
        double dvy = tSpd * std::cos(tHdg) - oSpd * std::cos(oHdg);
        double vSq = dvx*dvx + dvy*dvy;
        if (vSq > 0.001) {
            double tcpa = -(dLon*dvx + dLat*dvy) / vSq; // hours
            double cpx = dLon + dvx * tcpa;
            double cpy = dLat + dvy * tcpa;
            double cpa = std::sqrt(cpx*cpx + cpy*cpy);
            if (tcpa >= 0 && tcpa < 24) {
                ImVec4 cpaCol = (cpa < 0.5 && tcpa < 0.25) ? ImVec4(0.9f,0.1f,0.1f,1) : tc;
                ImGui::TextColored(cpaCol, "CPA %.2f nm  TCPA %.0f min", cpa, tcpa * 60);
            }
        }
    }

    if (ImGui::SmallButton("Close")) selectedAISIdx_ = -1;

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

// ============================================================================
// Compass rose
// ============================================================================

void EcdisDisplay::renderCompassRose(void* dlp, float cx, float cy, float r) {
    ImDrawList* dl = (ImDrawList*)dlp;
    bool night = (paletteIndex_ == 2);
    uint32_t ringCol = night ? COL(100, 100, 120, 150) : COL(60, 60, 80, 150);
    uint32_t northCol = night ? COL(200, 50, 50) : COL(200, 0, 0);
    uint32_t tickCol = night ? COL(140, 140, 160, 180) : COL(40, 40, 60, 180);
    uint32_t textCol = night ? COL(160, 160, 180) : COL(40, 40, 60);
    uint32_t lubberCol = night ? COL(255, 200, 0) : COL(255, 200, 0);

    dl->AddCircleFilled(ImVec2(cx, cy), r + 4, COL(0, 0, 0, night ? 120 : 80), 48);
    dl->AddCircle(ImVec2(cx, cy), r, ringCol, 48, 1.5f);

    // Rotation offset: rotate rose so that ship heading appears at top
    float rotOffset = -ownShip_.heading * (float)M_PI / 180.0f;

    const char* cardinals[] = { "N", nullptr, nullptr, "E", nullptr, nullptr,
                                 "S", nullptr, nullptr, "W", nullptr, nullptr };
    for (int i = 0; i < 12; i++) {
        float ang = i * 30.0f * (float)M_PI / 180.0f + rotOffset;
        float s = std::sin(ang), c2 = -std::cos(ang);
        float innerR = (i % 3 == 0) ? r * 0.7f : r * 0.85f;
        dl->AddLine(ImVec2(cx + innerR*s, cy + innerR*c2),
                    ImVec2(cx + r*s, cy + r*c2),
                    (i == 0) ? northCol : tickCol,
                    (i % 3 == 0) ? 2.0f : 1.0f);
        if (cardinals[i]) {
            float tx = cx + (r + 10)*s - 4;
            float ty = cy + (r + 10)*c2 - 7;
            dl->AddText(ImVec2(tx, ty), (i == 0) ? northCol : textCol, cardinals[i]);
        }
    }

    // North triangle (rotates with the rose)
    float nAng = rotOffset; // North is at 0 degrees true
    float nLen = r * 0.65f;
    float nTipX = cx + nLen * std::sin(nAng);
    float nTipY = cy - nLen * std::cos(nAng);
    float nBaseX = cx + nLen * 0.3f * std::sin(nAng);
    float nBaseY = cy - nLen * 0.3f * std::cos(nAng);
    float perpX = std::cos(nAng);
    float perpY = std::sin(nAng);
    dl->AddTriangleFilled(
        ImVec2(nTipX, nTipY),
        ImVec2(nBaseX - 5*perpX, nBaseY - 5*perpY),
        ImVec2(nBaseX + 5*perpX, nBaseY + 5*perpY),
        northCol);

    // Fixed lubber line at top (always points up = ship's heading)
    dl->AddTriangleFilled(
        ImVec2(cx, cy - r - 2),
        ImVec2(cx - 5, cy - r - 10),
        ImVec2(cx + 5, cy - r - 10),
        lubberCol);

    // Numeric heading in center
    char hdgBuf[16];
    snprintf(hdgBuf, sizeof(hdgBuf), "%03.0f", ownShip_.heading);
    ImVec2 hdgSize = ImGui::CalcTextSize(hdgBuf);
    dl->AddText(ImVec2(cx - hdgSize.x * 0.5f, cy - hdgSize.y * 0.5f), textCol, hdgBuf);
}

// ============================================================================
// Scale bar
// ============================================================================

void EcdisDisplay::renderScaleBar(void* dlp, float x, float y) {
    ImDrawList* dl = (ImDrawList*)dlp;
    bool night = (paletteIndex_ == 2);
    uint32_t barCol = night ? COL(180, 180, 200) : COL(0, 0, 0);
    uint32_t bgc = night ? COL(0, 0, 0, 140) : COL(255, 255, 255, 180);

    float mpp = metersPerPixel();
    if (mpp <= 0) return;

    float targetPx = 150.0f;
    float targetNm = targetPx * mpp / 1852.0f;

    float niceNm;
    if (targetNm >= 10) niceNm = std::round(targetNm / 5) * 5;
    else if (targetNm >= 1) niceNm = std::round(targetNm);
    else if (targetNm >= 0.1f) niceNm = std::round(targetNm * 10) / 10;
    else niceNm = std::round(targetNm * 100) / 100;
    if (niceNm <= 0) niceNm = 0.1f;

    float barPx = niceNm * 1852.0f / mpp;

    dl->AddRectFilled(ImVec2(x - 5, y - 18), ImVec2(x + barPx + 5, y + 5), bgc, 3.0f);
    dl->AddLine(ImVec2(x, y), ImVec2(x + barPx, y), barCol, 2.0f);
    dl->AddLine(ImVec2(x, y - 5), ImVec2(x, y + 3), barCol, 2.0f);
    dl->AddLine(ImVec2(x + barPx, y - 5), ImVec2(x + barPx, y + 3), barCol, 2.0f);

    char buf[32];
    if (niceNm >= 1.0f) snprintf(buf, sizeof(buf), "%.0f nm", niceNm);
    else snprintf(buf, sizeof(buf), "%.1f nm", niceNm);
    ImVec2 ts = ImGui::CalcTextSize(buf);
    dl->AddText(ImVec2(x + (barPx - ts.x) * 0.5f, y - 16), barCol, buf);
}

// ============================================================================
// Data box (top-left)
// ============================================================================

void EcdisDisplay::renderDataBox(float screenW) {
    bool night = (paletteIndex_ == 2);
    ImVec4 bg = night ? ImVec4(0.02f, 0.02f, 0.06f, 0.85f)
                       : ImVec4(1.0f, 1.0f, 1.0f, 0.88f);
    ImVec4 labelCol = night ? ImVec4(0.4f, 0.4f, 0.5f, 1) : ImVec4(0.4f, 0.4f, 0.4f, 1);
    ImVec4 valCol = night ? ImVec4(0.9f, 0.9f, 1.0f, 1) : ImVec4(0, 0, 0.15f, 1);

    ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(bg.w);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, bg);
    ImGui::PushStyleColor(ImGuiCol_Border, night ? ImVec4(0.2f,0.2f,0.3f,0.6f) : ImVec4(0.5f,0.5f,0.6f,0.6f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);

    ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

    ImGui::Begin("##DataBox", nullptr, wf);
    if (ImGui::IsWindowHovered()) floatingWindowHovered_ = true;

    {
        char ns = ownShip_.lat >= 0 ? 'N' : 'S';
        char ew = ownShip_.lon >= 0 ? 'E' : 'W';
        double aLat = std::fabs(ownShip_.lat), aLon = std::fabs(ownShip_.lon);
        int latD = (int)aLat; double latM = (aLat - latD) * 60.0;
        int lonD = (int)aLon; double lonM = (aLon - lonD) * 60.0;
        ImGui::TextColored(valCol, "%02d %06.3f'%c  %03d %06.3f'%c",
                           latD, latM, ns, lonD, lonM, ew);
    }

    ImGui::Spacing();

    ImGui::TextColored(labelCol, "HDG"); ImGui::SameLine(36);
    ImGui::TextColored(valCol, "%05.1f", ownShip_.heading);
    ImGui::SameLine(100); ImGui::TextColored(labelCol, "COG"); ImGui::SameLine(136);
    ImGui::TextColored(valCol, "%05.1f", ownShip_.cog);

    ImGui::TextColored(labelCol, "SOG"); ImGui::SameLine(36);
    ImGui::TextColored(valCol, "%4.1f kn", ownShip_.sog);
    ImGui::SameLine(100); ImGui::TextColored(labelCol, "STW"); ImGui::SameLine(136);
    ImGui::TextColored(valCol, "%4.1f kn", ownShip_.stw);

    ImVec4 depCol = valCol;
    if (ownShip_.depth < 5) depCol = ImVec4(0.9f, 0.1f, 0.1f, 1);
    else if (ownShip_.depth < 10) depCol = ImVec4(0.8f, 0.7f, 0, 1);
    ImGui::TextColored(labelCol, "DPT"); ImGui::SameLine(36);
    ImGui::TextColored(depCol, "%.1f m", ownShip_.depth);

    // Next waypoint info if route exists
    if (!routeWaypoints_.empty()) {
        ImGui::Spacing();
        ImGui::Separator();
        ImVec4 rteCol = night ? ImVec4(0.8f, 0.3f, 0.8f, 1) : ImVec4(0.6f, 0, 0.6f, 1);
        // Show bearing/distance to first (or next) waypoint
        int nextWP = 0; // TODO: track active waypoint
        float brg, dist;
        calcBearingDistance(ownShip_.lat, ownShip_.lon,
                          routeWaypoints_[nextWP].lat, routeWaypoints_[nextWP].lon, brg, dist);
        ImGui::TextColored(rteCol, "WP%02d %03.0f / %.2f nm", nextWP + 1, brg, dist);
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

// ============================================================================
// Toolbar
// ============================================================================

void EcdisDisplay::renderToolbar(float screenH) {
    bool night = (paletteIndex_ == 2);
    ImVec4 bg = night ? ImVec4(0.02f, 0.02f, 0.06f, 0.85f) : ImVec4(0.95f, 0.95f, 0.97f, 0.9f);
    ImVec4 btnCol = night ? ImVec4(0.15f, 0.15f, 0.2f, 1) : ImVec4(0.85f, 0.85f, 0.88f, 1);

    float tbW = 44.0f;
    float tbH = 380.0f;
    float tbY = (screenH - 28 - tbH) * 0.5f;

    ImGui::SetNextWindowPos(ImVec2(6, tbY), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(bg.w);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, bg);
    ImGui::PushStyleColor(ImGuiCol_Button, btnCol);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 6));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));

    ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

    ImGui::Begin("##Toolbar", nullptr, wf);
    if (ImGui::IsWindowHovered()) floatingWindowHovered_ = true;

    ImVec2 btnSz(36, 32);

    // Zoom
    if (ImGui::Button("+##zin", btnSz)) setZoom(zoom_ + 1);
    {
        char zb[8]; snprintf(zb, sizeof(zb), " %d", zoom_);
        ImGui::SetCursorPosX((tbW - ImGui::CalcTextSize(zb).x) * 0.5f - 2);
        ImGui::TextUnformatted(zb);
    }
    if (ImGui::Button("-##zout", btnSz)) setZoom(zoom_ - 1);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Center on ship
    bool wasCenter = centerOnShip_;
    if (wasCenter) {
        ImGui::PushStyleColor(ImGuiCol_Button, night ? ImVec4(0.1f, 0.3f, 0.1f, 1) : ImVec4(0.7f, 0.9f, 0.7f, 1));
    }
    if (ImGui::Button("CTR", btnSz)) {
        centerOnShip_ = true;
        centerLat_ = ownShip_.lat; centerLon_ = ownShip_.lon;
    }
    if (wasCenter) ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Route input mode toggle
    bool inRouteMode = (interactionMode_ == InteractionMode::AddWaypoint);
    if (inRouteMode) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.1f, 0.6f, 1));
    }
    if (ImGui::Button("RTE", btnSz)) {
        if (inRouteMode) {
            interactionMode_ = InteractionMode::Navigate;
        } else {
            interactionMode_ = InteractionMode::AddWaypoint;
            selectedAISIdx_ = -1;
        }
    }
    if (inRouteMode) ImGui::PopStyleColor();

    // Measurement tool
    bool inMsrMode = (interactionMode_ == InteractionMode::MeasureBearingDistance);
    if (inMsrMode) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.5f, 0.1f, 1));
    if (ImGui::Button("MSR", btnSz)) {
        if (inMsrMode) {
            interactionMode_ = InteractionMode::Navigate;
            activeMeasureStep_ = 0;
        } else {
            interactionMode_ = InteractionMode::MeasureBearingDistance;
            activeMeasureStep_ = 0;
        }
    }
    if (inMsrMode) ImGui::PopStyleColor();

    // Clear measurements
    if (!measurements_.empty()) {
        if (ImGui::Button("CML", btnSz)) { measurements_.clear(); }
    }

    // Marker placement
    bool inMrkMode = (interactionMode_ == InteractionMode::PlaceMarker);
    if (inMrkMode) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.4f, 0.5f, 1));
    if (ImGui::Button("MRK", btnSz)) {
        if (inMrkMode) {
            interactionMode_ = InteractionMode::Navigate;
        } else {
            interactionMode_ = InteractionMode::PlaceMarker;
        }
    }
    if (inMrkMode) ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // DR/EP toggle
    bool wasDREP = showDREP_;
    if (wasDREP) ImGui::PushStyleColor(ImGuiCol_Button, night ? ImVec4(0.1f, 0.1f, 0.3f, 1) : ImVec4(0.7f, 0.7f, 0.9f, 1));
    if (ImGui::Button("DR", btnSz)) { showDREP_ = !showDREP_; }
    if (wasDREP) ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Palette
    const char* palLabels[] = { "DAY", "DSK", "NIT" };
    if (ImGui::Button(palLabels[paletteIndex_], btnSz)) {
        paletteIndex_ = (paletteIndex_ + 1) % 3;
    }

    // Info toggle
    if (ImGui::Button(showDataBox_ ? "INF" : "inf", btnSz)) {
        showDataBox_ = !showDataBox_;
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Close
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.1f, 0.1f, 1));
    if (ImGui::Button("ESC", btnSz)) {
        closeRequested_ = true;
    }
    ImGui::PopStyleColor();

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

// ============================================================================
// Status bar
// ============================================================================

void EcdisDisplay::renderStatusBar(float x, float y, float w, float h) {
    bool night = (paletteIndex_ == 2);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    uint32_t bgc = night ? COL(10, 10, 20, 240) : COL(240, 240, 245, 245);
    uint32_t brd = night ? COL(40, 40, 60) : COL(160, 160, 180);
    uint32_t tc  = night ? COL(180, 180, 200) : COL(30, 30, 50);
    uint32_t lc  = night ? COL(80, 80, 100) : COL(100, 100, 120);

    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), bgc);
    dl->AddLine(ImVec2(x, y), ImVec2(x + w, y), brd);

    float ty = y + (h - 14) * 0.5f;
    float cx = x + 8;

    // Own position
    {
        char ns = ownShip_.lat >= 0 ? 'N' : 'S';
        char ew = ownShip_.lon >= 0 ? 'E' : 'W';
        double aLat = std::fabs(ownShip_.lat), aLon = std::fabs(ownShip_.lon);
        int ld = (int)aLat; double lm = (aLat - ld) * 60;
        int od = (int)aLon; double om = (aLon - od) * 60;
        char buf[64];
        snprintf(buf, sizeof(buf), "%02d %05.2f'%c %03d %05.2f'%c", ld, lm, ns, od, om, ew);
        dl->AddText(ImVec2(cx, ty), tc, buf);
        cx += ImGui::CalcTextSize(buf).x + 12;
    }

    // SOG
    {
        char buf[32]; snprintf(buf, sizeof(buf), "SOG %.1f kn", ownShip_.sog);
        dl->AddText(ImVec2(cx, ty), tc, buf);
        cx += ImGui::CalcTextSize(buf).x + 12;
    }

    // COG
    {
        char buf[32]; snprintf(buf, sizeof(buf), "COG %05.1f", ownShip_.cog);
        dl->AddText(ImVec2(cx, ty), tc, buf);
        cx += ImGui::CalcTextSize(buf).x + 20;
    }

    dl->AddLine(ImVec2(cx - 10, y + 4), ImVec2(cx - 10, y + h - 4), brd);

    // Mode indicator
    const char* modeText = nullptr;
    uint32_t modeCol = tc;
    if (interactionMode_ == InteractionMode::AddWaypoint) {
        modeText = "ROUTE INPUT - click to place WP, right-click to finish";
        modeCol = COL(255, 0, 255);
    } else if (interactionMode_ == InteractionMode::MeasureBearingDistance) {
        modeText = activeMeasureStep_ == 0 ? "MEASURE - click first point" : "MEASURE - click second point";
        modeCol = COL(255, 255, 0);
    } else if (interactionMode_ == InteractionMode::PlaceMarker) {
        modeText = "MARKER - click to place, right-click to finish";
        modeCol = COL(0, 200, 255);
    } else if (selectedWaypointIdx_ >= 0) {
        modeText = "WP selected - DEL to remove";
        modeCol = COL(255, 255, 0);
    } else if (selectedMarkerIdx_ >= 0) {
        modeText = "Marker selected - DEL to remove";
        modeCol = COL(0, 200, 255);
    }
    if (modeText) {
        dl->AddText(ImVec2(cx, ty), modeCol, modeText);
        cx += ImGui::CalcTextSize(modeText).x + 12;
    }

    // Cursor info
    ImGuiIO& io = ImGui::GetIO();
    float mx2 = io.MousePos.x, my2 = io.MousePos.y;
    bool mouseInChart = (mx2 >= chartX_ && mx2 < chartX_ + chartW_ &&
                         my2 >= chartY_ && my2 < chartY_ + chartH_);
    if (mouseInChart) {
        auto cl = screenToLatLon(mx2, my2);
        char ns = cl.lat >= 0 ? 'N' : 'S';
        char ew = cl.lon >= 0 ? 'E' : 'W';
        double aLat = std::fabs(cl.lat), aLon = std::fabs(cl.lon);
        char buf[64];
        snprintf(buf, sizeof(buf), "%02d %05.2f'%c %03d %05.2f'%c",
                 (int)aLat, (aLat-(int)aLat)*60, ns, (int)aLon, (aLon-(int)aLon)*60, ew);
        dl->AddText(ImVec2(cx, ty), lc, buf);
        cx += ImGui::CalcTextSize(buf).x + 12;

        float brg, rng;
        calcBearingDistance(ownShip_.lat, ownShip_.lon, cl.lat, cl.lon, brg, rng);
        char buf2[48];
        snprintf(buf2, sizeof(buf2), "BRG %05.1f  RNG %.2f nm", brg, rng);
        dl->AddText(ImVec2(cx, ty), lc, buf2);
    }

    // Scale (right side)
    float chartNm = chartW_ * metersPerPixel() / 1852.0f;
    char sb[32];
    snprintf(sb, sizeof(sb), "Z%d  %.1f nm", zoom_, chartNm);
    float sw = ImGui::CalcTextSize(sb).x;
    dl->AddText(ImVec2(x + w - sw - 10, ty), lc, sb);
}

// ============================================================================
// DR/EP projection
// ============================================================================

void EcdisDisplay::renderDREP(void* dlp) {
    ImDrawList* dl = (ImDrawList*)dlp;
    float stwMs = ownShip_.stw * 0.514444f; // knots to m/s
    if (stwMs < 0.1f) return;

    bool night = (paletteIndex_ == 2);
    uint32_t drCol = night ? COL(80, 80, 220) : COL(0, 0, 180);
    uint32_t epCol = night ? COL(220, 100, 50) : COL(200, 50, 0);
    uint32_t tidalCol = night ? COL(0, 180, 180) : COL(0, 140, 140);
    uint32_t textCol = night ? COL(120, 120, 200) : COL(0, 0, 160);
    uint32_t epTextCol = night ? COL(200, 120, 60) : COL(180, 50, 0);

    float totalSeconds = drepMinutes_ * 60.0f;
    float tickIntervalSec = 360.0f; // 6 minutes
    int numTicks = (int)(totalSeconds / tickIntervalSec);
    if (numTicks < 1) numTicks = 1;

    double hdgRad = ownShip_.heading * M_PI / 180.0;
    double cosLat = std::cos(ownShip_.lat * M_PI / 180.0);
    if (cosLat < 0.001) cosLat = 0.001;

    // Compute DR positions (rhumb line along heading at STW)
    double stwNmPerSec = stwMs / 1852.0;
    ScreenPos prevSP = latLonToScreen(ownShip_.lat, ownShip_.lon);
    double drLat = ownShip_.lat, drLon = ownShip_.lon;

    // Draw dashed DR line with 6-minute tick marks
    float dashOn = 8.0f, dashOff = 5.0f;
    float accum = 0;
    bool dashState = true;

    for (int tick = 1; tick <= numTicks; tick++) {
        double dt = tickIntervalSec;
        double dLatDeg = stwNmPerSec * std::cos(hdgRad) * dt / 60.0;
        double dLonDeg = stwNmPerSec * std::sin(hdgRad) * dt / (60.0 * cosLat);
        double nextLat = drLat + dLatDeg;
        double nextLon = drLon + dLonDeg;

        ScreenPos nextSP = latLonToScreen(nextLat, nextLon);

        // Draw dashed segment from prevSP to nextSP
        float segLen = std::sqrt((nextSP.x-prevSP.x)*(nextSP.x-prevSP.x) +
                                  (nextSP.y-prevSP.y)*(nextSP.y-prevSP.y));
        if (segLen > 1.0f) {
            float dx = (nextSP.x - prevSP.x) / segLen;
            float dy = (nextSP.y - prevSP.y) / segLen;
            float pos = 0;
            while (pos < segLen) {
                float remain = dashState ? dashOn - accum : dashOff - accum;
                float draw = std::min(remain, segLen - pos);
                if (dashState) {
                    dl->AddLine(ImVec2(prevSP.x + dx*(pos), prevSP.y + dy*(pos)),
                                ImVec2(prevSP.x + dx*(pos+draw), prevSP.y + dy*(pos+draw)),
                                drCol, 1.5f);
                }
                pos += draw;
                accum += draw;
                if (dashState && accum >= dashOn) { dashState = false; accum = 0; }
                else if (!dashState && accum >= dashOff) { dashState = true; accum = 0; }
            }
        }

        // Tick mark (perpendicular) at each 6-min interval
        float perpX = -(nextSP.y - prevSP.y) / (segLen > 0 ? segLen : 1.0f);
        float perpY =  (nextSP.x - prevSP.x) / (segLen > 0 ? segLen : 1.0f);
        float tickLen = 6.0f;
        dl->AddLine(ImVec2(nextSP.x - perpX*tickLen, nextSP.y - perpY*tickLen),
                    ImVec2(nextSP.x + perpX*tickLen, nextSP.y + perpY*tickLen),
                    drCol, 1.5f);

        drLat = nextLat; drLon = nextLon;
        prevSP = nextSP;
    }

    // DR endpoint label
    ScreenPos drEnd = latLonToScreen(drLat, drLon);
    char drLabel[32]; snprintf(drLabel, sizeof(drLabel), "DR +%.0f min", drepMinutes_);
    dl->AddText(ImVec2(drEnd.x + 8, drEnd.y - 8), textCol, drLabel);

    // EP: DR position + tidal vector
    float tidalX = ownShip_.tidalStreamX; // m/s east
    float tidalZ = ownShip_.tidalStreamZ; // m/s north
    bool hasTidal = (std::fabs(tidalX) > 0.001f || std::fabs(tidalZ) > 0.001f);
    if (hasTidal) {
        double tidalNmPerSec = 1.0 / 1852.0;
        double epLat = drLat + tidalZ * tidalNmPerSec * totalSeconds / 60.0;
        double epLon = drLon + tidalX * tidalNmPerSec * totalSeconds / (60.0 * cosLat);

        ScreenPos epSP = latLonToScreen(epLat, epLon);

        // Dashed line from DR to EP (tidal component)
        dl->AddLine(ImVec2(drEnd.x, drEnd.y), ImVec2(epSP.x, epSP.y), epCol, 1.5f);

        // EP diamond
        float d = 6.0f;
        dl->AddQuad(ImVec2(epSP.x, epSP.y - d), ImVec2(epSP.x + d, epSP.y),
                    ImVec2(epSP.x, epSP.y + d), ImVec2(epSP.x - d, epSP.y), epCol, 2.0f);

        char epLabel[32]; snprintf(epLabel, sizeof(epLabel), "EP +%.0f min", drepMinutes_);
        dl->AddText(ImVec2(epSP.x + 8, epSP.y - 8), epTextCol, epLabel);

        // Tidal arrow at ship position
        ScreenPos shipSP = latLonToScreen(ownShip_.lat, ownShip_.lon);
        float tidalSpd = std::sqrt(tidalX*tidalX + tidalZ*tidalZ);
        float tidalPx = nmToPixels((float)(tidalSpd * 1.94384 * 0.1)); // 6-min vector
        if (tidalPx > 3.0f) {
            float tidalDir = std::atan2(tidalX, tidalZ); // radians, 0=N
            float tx = shipSP.x + tidalPx * std::sin(tidalDir);
            float ty = shipSP.y - tidalPx * std::cos(tidalDir);
            dl->AddLine(ImVec2(shipSP.x, shipSP.y), ImVec2(tx, ty), tidalCol, 2.0f);
            // Arrow head
            float aLen = 6.0f, aAng = 0.4f;
            float dx = tx - shipSP.x, dy = ty - shipSP.y;
            float ang = std::atan2(dy, dx);
            dl->AddLine(ImVec2(tx, ty),
                        ImVec2(tx - aLen*std::cos(ang-aAng), ty - aLen*std::sin(ang-aAng)),
                        tidalCol, 2.0f);
            dl->AddLine(ImVec2(tx, ty),
                        ImVec2(tx - aLen*std::cos(ang+aAng), ty - aLen*std::sin(ang+aAng)),
                        tidalCol, 2.0f);
        }
    }
}

// ============================================================================
// Measurement lines
// ============================================================================

void EcdisDisplay::renderMeasurements(void* dlp) {
    ImDrawList* dl = (ImDrawList*)dlp;
    if (measurements_.empty()) return;

    bool night = (paletteIndex_ == 2);
    uint32_t lineCol = night ? COL(255, 255, 100) : COL(180, 150, 0);
    uint32_t textCol = night ? COL(255, 255, 150) : COL(140, 120, 0);
    uint32_t dotCol  = night ? COL(255, 255, 0) : COL(200, 160, 0);

    for (const auto& m : measurements_) {
        ScreenPos a = latLonToScreen(m.lat1, m.lon1);
        ScreenPos b = latLonToScreen(m.lat2, m.lon2);

        dl->AddLine(ImVec2(a.x, a.y), ImVec2(b.x, b.y), lineCol, 1.5f);
        dl->AddCircleFilled(ImVec2(a.x, a.y), 3.0f, dotCol);
        dl->AddCircleFilled(ImVec2(b.x, b.y), 3.0f, dotCol);

        float brg, dist;
        calcBearingDistance(m.lat1, m.lon1, m.lat2, m.lon2, brg, dist);
        float midX = (a.x + b.x) * 0.5f, midY = (a.y + b.y) * 0.5f;
        char buf[48]; snprintf(buf, sizeof(buf), "%03.0f / %.2f nm", brg, dist);
        dl->AddText(ImVec2(midX + 5, midY - 14), textCol, buf);
    }
}

// ============================================================================
// Chart markers
// ============================================================================

void EcdisDisplay::renderMarkers(void* dlp) {
    ImDrawList* dl = (ImDrawList*)dlp;
    if (chartMarkers_.empty()) return;

    bool night = (paletteIndex_ == 2);
    uint32_t mkCol = night ? COL(0, 200, 255) : COL(0, 140, 200);
    uint32_t selCol = COL(255, 255, 0);
    uint32_t textCol = night ? COL(0, 180, 230) : COL(0, 100, 160);

    for (int i = 0; i < (int)chartMarkers_.size(); i++) {
        const auto& mk = chartMarkers_[i];
        ScreenPos p = latLonToScreen(mk.lat, mk.lon);
        if (p.x < chartX_ - 50 || p.x > chartX_ + chartW_ + 50 ||
            p.y < chartY_ - 50 || p.y > chartY_ + chartH_ + 50) continue;

        bool selected = (i == selectedMarkerIdx_);
        uint32_t col = selected ? selCol : mkCol;
        float d = selected ? 7.0f : 5.0f;

        // Diamond shape
        dl->AddQuad(ImVec2(p.x, p.y - d), ImVec2(p.x + d, p.y),
                    ImVec2(p.x, p.y + d), ImVec2(p.x - d, p.y), col, 2.0f);
        if (selected)
            dl->AddQuadFilled(ImVec2(p.x, p.y - d), ImVec2(p.x + d, p.y),
                              ImVec2(p.x, p.y + d), ImVec2(p.x - d, p.y), COL(255, 255, 0, 40));

        dl->AddText(ImVec2(p.x + d + 3, p.y - 7), selected ? selCol : textCol, mk.label.c_str());
    }
}

}} // namespace bc::gui

#endif // WITH_WICKED_ENGINE
