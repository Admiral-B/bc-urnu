#include "MapWidget.hpp"
#include "TileDownloader.hpp"
#include "TileTextureManager.hpp"
#include <irrlicht.h>
#include <cmath>
#include <sstream>
#include <iomanip>

using namespace irr;

MapWidget::MapWidget(TileDownloader* downloader, TileTextureManager* texManager)
    : downloader(downloader)
    , texManager(texManager)
{
}

MapWidget::~MapWidget() {
}

void MapWidget::setCenter(double lat, double lon) {
    centerLat = TileMath::clampLat(lat);
    centerLon = lon;
    // Wrap longitude to [-180, 180]
    while (centerLon > 180.0) centerLon -= 360.0;
    while (centerLon < -180.0) centerLon += 360.0;
}

void MapWidget::setZoom(int z) {
    zoom = std::max(2, std::min(19, z));
}

TileMath::LatLon MapWidget::screenToLatLon(int sx, int sy) const {
    int offsetX = sx - widgetW / 2;
    int offsetY = sy - widgetH / 2;
    return TileMath::pixelToLatLon(centerLat, centerLon, zoom, offsetX, offsetY);
}

TileMath::PixelPos MapWidget::latLonToScreen(double lat, double lon) const {
    TileMath::PixelPos px = TileMath::latLonToPixel(centerLat, centerLon, zoom, lat, lon);
    return {px.x + widgetW / 2, px.y + widgetH / 2};
}

void MapWidget::render(video::IVideoDriver* driver, gui::IGUIFont* font,
                       int screenX, int screenY, int screenW, int screenH) {
    widgetX = screenX;
    widgetY = screenY;
    widgetW = screenW;
    widgetH = screenH;

    texManager->beginFrame();

    // Clip rendering to widget area
    driver->enableClipPlane(0, false); // Irrlicht doesn't have 2D clip, we'll check bounds manually

    // Draw background (dark blue for ocean areas that haven't loaded)
    driver->draw2DRectangle(video::SColor(255, 20, 30, 50),
        core::rect<s32>(screenX, screenY, screenX + screenW, screenY + screenH));

    renderTiles(driver);

    if (showGrid) {
        renderGridLines(driver, font);
    }

    if (showOverlay) {
        renderOverlay(driver, font);
    }

    // Evict unused textures
    texManager->evictUnused(300);
}

void MapWidget::renderTiles(video::IVideoDriver* driver) {
    const int tileSize = 256;

    // Compute the global pixel coordinates of the map center
    double centerPxX = TileMath::lonToPixelX(centerLon, zoom, tileSize);
    double centerPxY = TileMath::latToPixelY(centerLat, zoom, tileSize);

    // Compute the global pixel coordinates of the widget's top-left corner
    double topLeftPxX = centerPxX - widgetW / 2.0;
    double topLeftPxY = centerPxY - widgetH / 2.0;

    // Compute tile range that covers the widget
    int tileXStart = static_cast<int>(std::floor(topLeftPxX / tileSize));
    int tileYStart = static_cast<int>(std::floor(topLeftPxY / tileSize));
    int tileXEnd = static_cast<int>(std::floor((topLeftPxX + widgetW) / tileSize));
    int tileYEnd = static_cast<int>(std::floor((topLeftPxY + widgetH) / tileSize));

    int maxTile = (1 << zoom) - 1;

    for (int ty = tileYStart; ty <= tileYEnd; ty++) {
        for (int tx = tileXStart; tx <= tileXEnd; tx++) {
            // Wrap tile X for world wrapping
            int wrappedTX = tx;
            while (wrappedTX < 0) wrappedTX += (1 << zoom);
            while (wrappedTX > maxTile) wrappedTX -= (1 << zoom);

            // Skip tiles outside valid Y range
            if (ty < 0 || ty > maxTile) continue;

            // Compute screen position of this tile
            int screenTileX = static_cast<int>(tx * tileSize - topLeftPxX) + widgetX;
            int screenTileY = static_cast<int>(ty * tileSize - topLeftPxY) + widgetY;

            // Skip tiles fully outside widget
            if (screenTileX + tileSize < widgetX || screenTileX > widgetX + widgetW) continue;
            if (screenTileY + tileSize < widgetY || screenTileY > widgetY + widgetH) continue;

            // Get tile texture
            video::ITexture* tex = texManager->getTileTexture(zoom, wrappedTX, ty);
            if (tex) {
                core::rect<s32> destRect(screenTileX, screenTileY,
                    screenTileX + tileSize, screenTileY + tileSize);
                core::rect<s32> srcRect(0, 0, 256, 256);
                driver->draw2DImage(tex, destRect, srcRect, nullptr, nullptr, true);
            } else {
                // Placeholder: slightly lighter rectangle while loading
                driver->draw2DRectangle(video::SColor(255, 40, 50, 70),
                    core::rect<s32>(screenTileX, screenTileY,
                        screenTileX + tileSize, screenTileY + tileSize));
            }
        }
    }
}

void MapWidget::renderGridLines(video::IVideoDriver* driver, gui::IGUIFont* font) {
    // Grid spacing depends on zoom level
    double gridSpacing;
    if (zoom < 5) gridSpacing = 10.0;
    else if (zoom < 8) gridSpacing = 1.0;
    else if (zoom < 12) gridSpacing = 0.1;
    else if (zoom < 15) gridSpacing = 0.01;
    else gridSpacing = 0.001;

    // Get visible lat/lon bounds
    TileMath::LatLon topLeft = screenToLatLon(0, 0);
    TileMath::LatLon bottomRight = screenToLatLon(widgetW, widgetH);

    double minLon = std::min(topLeft.lon, bottomRight.lon);
    double maxLon = std::max(topLeft.lon, bottomRight.lon);
    double minLat = std::min(topLeft.lat, bottomRight.lat);
    double maxLat = std::max(topLeft.lat, bottomRight.lat);

    video::SColor gridColor(80, 255, 255, 255);
    video::SColor labelColor(200, 255, 255, 255);

    // Draw longitude lines (vertical)
    double lonStart = std::floor(minLon / gridSpacing) * gridSpacing;
    for (double lon = lonStart; lon <= maxLon; lon += gridSpacing) {
        TileMath::PixelPos top = latLonToScreen(maxLat, lon);
        TileMath::PixelPos bot = latLonToScreen(minLat, lon);
        int sx = top.x + widgetX;
        if (sx >= widgetX && sx <= widgetX + widgetW) {
            driver->draw2DLine(
                core::position2d<s32>(sx, widgetY),
                core::position2d<s32>(sx, widgetY + widgetH),
                gridColor);

            if (font) {
                std::ostringstream oss;
                oss << std::fixed << std::setprecision(gridSpacing < 0.1 ? 3 : (gridSpacing < 1.0 ? 1 : 0));
                oss << lon;
                core::stringw label(oss.str().c_str());
                font->draw(label, core::rect<s32>(sx + 2, widgetY + 2, sx + 100, widgetY + 20), labelColor);
            }
        }
    }

    // Draw latitude lines (horizontal)
    double latStart = std::floor(minLat / gridSpacing) * gridSpacing;
    for (double lat = latStart; lat <= maxLat; lat += gridSpacing) {
        TileMath::PixelPos left = latLonToScreen(lat, minLon);
        TileMath::PixelPos right = latLonToScreen(lat, maxLon);
        int sy = left.y + widgetY;
        if (sy >= widgetY && sy <= widgetY + widgetH) {
            driver->draw2DLine(
                core::position2d<s32>(widgetX, sy),
                core::position2d<s32>(widgetX + widgetW, sy),
                gridColor);

            if (font) {
                std::ostringstream oss;
                oss << std::fixed << std::setprecision(gridSpacing < 0.1 ? 3 : (gridSpacing < 1.0 ? 1 : 0));
                oss << lat;
                core::stringw label(oss.str().c_str());
                font->draw(label, core::rect<s32>(widgetX + 2, sy + 2, widgetX + 100, sy + 20), labelColor);
            }
        }
    }
}

void MapWidget::renderOverlay(video::IVideoDriver* driver, gui::IGUIFont* font) {
    if (!font) return;

    // Update mouse lat/lon
    TileMath::LatLon mouseLL = screenToLatLon(lastMouseX, lastMouseY);
    mouseLat = mouseLL.lat;
    mouseLon = mouseLL.lon;

    // Bottom status bar
    int barY = widgetY + widgetH - 22;
    driver->draw2DRectangle(video::SColor(180, 0, 0, 0),
        core::rect<s32>(widgetX, barY, widgetX + widgetW, widgetY + widgetH));

    // Format coordinates
    char ns = mouseLat >= 0 ? 'N' : 'S';
    char ew = mouseLon >= 0 ? 'E' : 'W';
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(5)
        << std::abs(mouseLat) << "\xC2\xB0" << ns << "  "
        << std::abs(mouseLon) << "\xC2\xB0" << ew
        << "   Zoom: " << zoom;

    int pending = downloader->pendingDownloads();
    if (pending > 0) {
        oss << "   Loading: " << pending << " tiles";
    }

    core::stringw statusText(oss.str().c_str());
    font->draw(statusText,
        core::rect<s32>(widgetX + 5, barY + 3, widgetX + widgetW - 5, widgetY + widgetH),
        video::SColor(255, 200, 200, 200));

    // Crosshair at center
    int cx = widgetX + widgetW / 2;
    int cy = widgetY + widgetH / 2;
    video::SColor crossColor(150, 255, 255, 0);
    driver->draw2DLine(core::position2d<s32>(cx - 10, cy), core::position2d<s32>(cx + 10, cy), crossColor);
    driver->draw2DLine(core::position2d<s32>(cx, cy - 10), core::position2d<s32>(cx, cy + 10), crossColor);

    // Scale bar at bottom-right
    // Compute approximate metres per pixel at center latitude
    double metersPerPixel = 156543.03392 * std::cos(centerLat * M_PI / 180.0) / (1 << zoom);
    double scaleBarMeters;
    if (metersPerPixel * 100 > 10000) scaleBarMeters = 10000;
    else if (metersPerPixel * 100 > 5000) scaleBarMeters = 5000;
    else if (metersPerPixel * 100 > 1000) scaleBarMeters = 1000;
    else if (metersPerPixel * 100 > 500) scaleBarMeters = 500;
    else if (metersPerPixel * 100 > 100) scaleBarMeters = 100;
    else scaleBarMeters = 50;

    int scaleBarPx = static_cast<int>(scaleBarMeters / metersPerPixel);
    int sbx = widgetX + widgetW - scaleBarPx - 20;
    int sby = barY - 15;

    driver->draw2DLine(core::position2d<s32>(sbx, sby), core::position2d<s32>(sbx + scaleBarPx, sby),
        video::SColor(255, 255, 255, 255));
    driver->draw2DLine(core::position2d<s32>(sbx, sby - 3), core::position2d<s32>(sbx, sby + 3),
        video::SColor(255, 255, 255, 255));
    driver->draw2DLine(core::position2d<s32>(sbx + scaleBarPx, sby - 3),
        core::position2d<s32>(sbx + scaleBarPx, sby + 3),
        video::SColor(255, 255, 255, 255));

    // Scale label
    std::ostringstream scaleOss;
    if (scaleBarMeters >= 1852) {
        scaleOss << std::fixed << std::setprecision(1) << (scaleBarMeters / 1852.0) << " NM";
    } else if (scaleBarMeters >= 1000) {
        scaleOss << static_cast<int>(scaleBarMeters / 1000) << " km";
    } else {
        scaleOss << static_cast<int>(scaleBarMeters) << " m";
    }

    core::stringw scaleLabel(scaleOss.str().c_str());
    font->draw(scaleLabel,
        core::rect<s32>(sbx, sby - 15, sbx + scaleBarPx, sby - 2),
        video::SColor(255, 255, 255, 255));
}

bool MapWidget::onEvent(const SEvent& event, int screenX, int screenY, int screenW, int screenH) {
    widgetX = screenX;
    widgetY = screenY;
    widgetW = screenW;
    widgetH = screenH;

    if (event.EventType == irr::EET_MOUSE_INPUT_EVENT) {
        int mx = event.MouseInput.X - widgetX;
        int my = event.MouseInput.Y - widgetY;

        // Check if mouse is within widget
        if (mx < 0 || mx > widgetW || my < 0 || my > widgetH) {
            if (isDragging && event.MouseInput.Event == irr::EMIE_LMOUSE_LEFT_UP) {
                isDragging = false;
            }
            return false;
        }

        lastMouseX = mx;
        lastMouseY = my;

        switch (event.MouseInput.Event) {
        case irr::EMIE_MOUSE_WHEEL: {
            // Zoom in/out, centered on mouse position
            TileMath::LatLon mouseLL = screenToLatLon(mx, my);
            if (event.MouseInput.Wheel > 0) {
                setZoom(zoom + 1);
            } else {
                setZoom(zoom - 1);
            }
            // Re-center so the point under the mouse stays in place
            TileMath::LatLon newMouseLL = screenToLatLon(mx, my);
            centerLat += mouseLL.lat - newMouseLL.lat;
            centerLon += mouseLL.lon - newMouseLL.lon;
            return true;
        }

        case irr::EMIE_LMOUSE_PRESSED_DOWN: {
            isDragging = true;
            dragStartMouseX = event.MouseInput.X;
            dragStartMouseY = event.MouseInput.Y;
            dragStartLat = centerLat;
            dragStartLon = centerLon;
            return true;
        }

        case irr::EMIE_LMOUSE_LEFT_UP: {
            isDragging = false;
            return true;
        }

        case irr::EMIE_MOUSE_MOVED: {
            if (isDragging) {
                // Compute pixel delta
                int dx = event.MouseInput.X - dragStartMouseX;
                int dy = event.MouseInput.Y - dragStartMouseY;

                // Convert pixel delta to lat/lon delta
                // At zoom level z, each pixel covers:
                double metersPerPixel = 156543.03392 * std::cos(dragStartLat * M_PI / 180.0) / (1 << zoom);
                double lonPerPixel = 360.0 / ((1 << zoom) * 256.0);
                double latPerPixel = metersPerPixel / 111320.0; // Approximate

                centerLon = dragStartLon - dx * lonPerPixel;
                centerLat = dragStartLat + dy * latPerPixel;
                centerLat = TileMath::clampLat(centerLat);

                if (onMapHover) {
                    TileMath::LatLon ll = screenToLatLon(mx, my);
                    onMapHover(ll.lat, ll.lon);
                }
                return true;
            }

            if (onMapHover) {
                TileMath::LatLon ll = screenToLatLon(mx, my);
                onMapHover(ll.lat, ll.lon);
            }
            return false;
        }

        case irr::EMIE_RMOUSE_PRESSED_DOWN: {
            if (onMapClick) {
                TileMath::LatLon ll = screenToLatLon(mx, my);
                onMapClick(ll.lat, ll.lon, 1); // right-click
            }
            return true;
        }

        default:
            break;
        }
    }

    return false;
}
