#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class TileDownloader;

class MapScreen {
public:
    struct AISContact {
        double lat, lon;
        float heading; // degrees, 0=N clockwise
        float speed;   // knots
    };

    MapScreen(int texSize = 512);
    ~MapScreen();

    void init(const std::string& cacheDir);

    // Update pixel buffer with current ship position and AIS contacts.
    void update(double ownLat, double ownLon, float ownHeading,
                const AISContact* contacts, int numContacts,
                int zoom = 14);

    const uint8_t* getPixels() const { return pixels.get(); }
    int getTexSize() const { return texSize; }

private:
    int texSize;
    std::unique_ptr<uint8_t[]> pixels;
    std::unique_ptr<TileDownloader> osmDownloader;
    std::unique_ptr<TileDownloader> seamarkDownloader;

    void compositeTiles(double centerLat, double centerLon, int zoom);
    void blitTile(int destX, int destY, const uint8_t* rgb, int tw, int th, bool alphaBlend);
    void drawOwnShip(int cx, int cy, float heading);
    void drawAISTarget(int x, int y, float heading, float speed);
    void drawLine(int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t b);
    void drawCircle(int cx, int cy, int radius, uint8_t r, uint8_t g, uint8_t b);
    void fillCircle(int cx, int cy, int radius, uint8_t r, uint8_t g, uint8_t b);
    void setPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);
    void blendPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
};
