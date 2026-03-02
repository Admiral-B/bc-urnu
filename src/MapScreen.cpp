#include "MapScreen.hpp"
#include "editor/TileDownloader.hpp"
#include "editor/TileMath.hpp"
#include "libs/stb/stb_image.h"

#include <cstring>
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

MapScreen::MapScreen(int texSize) : texSize(texSize) {
    pixels = std::make_unique<uint8_t[]>(texSize * texSize * 4);
    memset(pixels.get(), 0, texSize * texSize * 4);
}

MapScreen::~MapScreen() = default;

void MapScreen::init(const std::string& cacheDir) {
    osmDownloader = std::make_unique<TileDownloader>(
        "https://tile.openstreetmap.org/{z}/{x}/{y}.png",
        cacheDir + "osm/");
    osmDownloader->setUserAgent("BridgeCommand/6.0 (map-screen)");

    seamarkDownloader = std::make_unique<TileDownloader>(
        "https://tiles.openseamap.org/seamark/{z}/{x}/{y}.png",
        cacheDir + "seamark/");
    seamarkDownloader->setUserAgent("BridgeCommand/6.0 (map-screen)");
}

void MapScreen::update(double ownLat, double ownLon, float ownHeading,
                        const AISContact* contacts, int numContacts,
                        int zoom) {
    // Clear to ECDIS night palette background
    uint8_t* p = pixels.get();
    for (int i = 0; i < texSize * texSize; i++) {
        p[i * 4 + 0] = 0x0A; // R
        p[i * 4 + 1] = 0x16; // G
        p[i * 4 + 2] = 0x28; // B
        p[i * 4 + 3] = 0xFF; // A
    }

    // Composite map tiles centered on own ship
    compositeTiles(ownLat, ownLon, zoom);

    // Draw AIS targets
    int halfTex = texSize / 2;
    for (int i = 0; i < numContacts; i++) {
        TileMath::PixelPos pos = TileMath::latLonToPixel(
            ownLat, ownLon, zoom, contacts[i].lat, contacts[i].lon);
        int sx = halfTex + pos.x;
        int sy = halfTex + pos.y;
        if (sx >= -20 && sx < texSize + 20 && sy >= -20 && sy < texSize + 20) {
            drawAISTarget(sx, sy, contacts[i].heading, contacts[i].speed);
        }
    }

    // Draw own ship at center
    drawOwnShip(halfTex, halfTex, ownHeading);
}

void MapScreen::compositeTiles(double centerLat, double centerLon, int zoom) {
    if (!osmDownloader) return;

    const int tileSize = 256;

    // Own ship position in global pixel coordinates
    double centerPxX = TileMath::lonToPixelX(centerLon, zoom, tileSize);
    double centerPxY = TileMath::latToPixelY(centerLat, zoom, tileSize);

    // Top-left corner of our viewport in global pixel coords
    double vpLeftPx = centerPxX - texSize / 2.0;
    double vpTopPx = centerPxY - texSize / 2.0;

    // Which tiles cover this viewport
    int tileMinX = (int)std::floor(vpLeftPx / tileSize);
    int tileMinY = (int)std::floor(vpTopPx / tileSize);
    int tileMaxX = (int)std::floor((vpLeftPx + texSize) / tileSize);
    int tileMaxY = (int)std::floor((vpTopPx + texSize) / tileSize);

    int maxTile = (1 << zoom) - 1;

    // Blit each tile
    for (int ty = tileMinY; ty <= tileMaxY; ty++) {
        for (int tx = tileMinX; tx <= tileMaxX; tx++) {
            // Wrap tile X for world wrapping
            int wrappedTx = ((tx % (maxTile + 1)) + (maxTile + 1)) % (maxTile + 1);
            int clampedTy = std::max(0, std::min(maxTile, ty));

            // Where does this tile start in our viewport?
            int destX = (int)(tx * tileSize - vpLeftPx);
            int destY = (int)(ty * tileSize - vpTopPx);

            // Get base OSM tile
            std::vector<uint8_t> tileData = osmDownloader->getTile(zoom, wrappedTx, clampedTy);
            if (!tileData.empty()) {
                int tw = 0, th = 0, tc = 0;
                unsigned char* decoded = stbi_load_from_memory(
                    tileData.data(), (int)tileData.size(), &tw, &th, &tc, 3);
                if (decoded) {
                    blitTile(destX, destY, decoded, tw, th, false);
                    stbi_image_free(decoded);
                }
            }

            // Get seamark overlay tile (alpha-blended)
            if (seamarkDownloader) {
                std::vector<uint8_t> seamarkData = seamarkDownloader->getTile(zoom, wrappedTx, clampedTy);
                if (!seamarkData.empty()) {
                    int tw = 0, th = 0, tc = 0;
                    unsigned char* decoded = stbi_load_from_memory(
                        seamarkData.data(), (int)seamarkData.size(), &tw, &th, &tc, 4);
                    if (decoded) {
                        blitTile(destX, destY, decoded, tw, th, true);
                        stbi_image_free(decoded);
                    }
                }
            }
        }
    }
}

void MapScreen::blitTile(int destX, int destY, const uint8_t* data,
                          int tw, int th, bool alphaBlend) {
    for (int y = 0; y < th; y++) {
        int dy = destY + y;
        if (dy < 0 || dy >= texSize) continue;
        for (int x = 0; x < tw; x++) {
            int dx = destX + x;
            if (dx < 0 || dx >= texSize) continue;

            if (alphaBlend) {
                // RGBA source
                int si = (y * tw + x) * 4;
                uint8_t a = data[si + 3];
                if (a == 0) continue;
                blendPixel(dx, dy, data[si], data[si + 1], data[si + 2], a);
            } else {
                // RGB source (opaque)
                int si = (y * tw + x) * 3;
                setPixel(dx, dy, data[si], data[si + 1], data[si + 2], 255);
            }
        }
    }
}

void MapScreen::drawOwnShip(int cx, int cy, float heading) {
    // Yellow circle with heading line
    fillCircle(cx, cy, 5, 255, 200, 0);
    drawCircle(cx, cy, 5, 255, 255, 0);

    // Heading line (20 pixels long)
    float headRad = heading * (float)M_PI / 180.0f;
    int hx = cx + (int)(20.0f * sinf(headRad));
    int hy = cy - (int)(20.0f * cosf(headRad));
    drawLine(cx, cy, hx, hy, 255, 255, 0);
}

void MapScreen::drawAISTarget(int x, int y, float heading, float speed) {
    // Green triangle pointing in heading direction
    float headRad = heading * (float)M_PI / 180.0f;
    float sinH = sinf(headRad), cosH = cosf(headRad);

    // Triangle vertices (pointing in heading direction)
    int tipX = x + (int)(8.0f * sinH);
    int tipY = y - (int)(8.0f * cosH);
    int leftX = x + (int)(4.0f * (-cosH - sinH));
    int leftY = y + (int)(4.0f * (-sinH + cosH));
    int rightX = x + (int)(4.0f * (cosH - sinH));
    int rightY = y + (int)(4.0f * (sinH + cosH));

    drawLine(tipX, tipY, leftX, leftY, 0, 220, 0);
    drawLine(leftX, leftY, rightX, rightY, 0, 220, 0);
    drawLine(rightX, rightY, tipX, tipY, 0, 220, 0);

    // Speed vector line (1 pixel per knot, max 20)
    if (speed > 0.5f) {
        float vecLen = std::min(speed, 20.0f);
        int vx = x + (int)(vecLen * sinH);
        int vy = y - (int)(vecLen * cosH);
        drawLine(x, y, vx, vy, 0, 180, 0);
    }
}

// Bresenham line drawing
void MapScreen::drawLine(int x0, int y0, int x1, int y1,
                          uint8_t r, uint8_t g, uint8_t b) {
    int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (true) {
        setPixel(x0, y0, r, g, b, 255);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void MapScreen::drawCircle(int cx, int cy, int radius,
                            uint8_t r, uint8_t g, uint8_t b) {
    int x = radius, y = 0, err = 1 - radius;
    while (x >= y) {
        setPixel(cx + x, cy + y, r, g, b, 255);
        setPixel(cx - x, cy + y, r, g, b, 255);
        setPixel(cx + x, cy - y, r, g, b, 255);
        setPixel(cx - x, cy - y, r, g, b, 255);
        setPixel(cx + y, cy + x, r, g, b, 255);
        setPixel(cx - y, cy + x, r, g, b, 255);
        setPixel(cx + y, cy - x, r, g, b, 255);
        setPixel(cx - y, cy - x, r, g, b, 255);
        y++;
        if (err < 0) { err += 2 * y + 1; }
        else { x--; err += 2 * (y - x) + 1; }
    }
}

void MapScreen::fillCircle(int cx, int cy, int radius,
                            uint8_t r, uint8_t g, uint8_t b) {
    for (int dy = -radius; dy <= radius; dy++) {
        int dx = (int)std::sqrt((float)(radius * radius - dy * dy));
        for (int x = cx - dx; x <= cx + dx; x++) {
            setPixel(x, cy + dy, r, g, b, 255);
        }
    }
}

void MapScreen::setPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (x < 0 || x >= texSize || y < 0 || y >= texSize) return;
    int idx = (y * texSize + x) * 4;
    pixels[idx + 0] = r;
    pixels[idx + 1] = g;
    pixels[idx + 2] = b;
    pixels[idx + 3] = a;
}

void MapScreen::blendPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (x < 0 || x >= texSize || y < 0 || y >= texSize) return;
    if (a == 255) { setPixel(x, y, r, g, b, 255); return; }
    int idx = (y * texSize + x) * 4;
    float af = a / 255.0f;
    float inv = 1.0f - af;
    pixels[idx + 0] = (uint8_t)(r * af + pixels[idx + 0] * inv);
    pixels[idx + 1] = (uint8_t)(g * af + pixels[idx + 1] * inv);
    pixels[idx + 2] = (uint8_t)(b * af + pixels[idx + 2] * inv);
    pixels[idx + 3] = 255;
}
