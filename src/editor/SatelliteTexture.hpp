#pragma once

#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>

class TileDownloader;

// Downloads ESRI satellite imagery tiles for a bounding box and stitches them
// into a single RGB texture suitable for Bridge Command terrain rendering.
class SatelliteTexture {
public:
    // Progress callback: (tilesCompleted, totalTiles) -> should return false to cancel
    using ProgressCallback = std::function<bool(int, int)>;

    // Download satellite tiles and stitch into an RGB texture.
    // Returns 3-byte-per-pixel RGB data (row-major, top-to-bottom = north-to-south).
    // outputWidth/outputHeight receive the raw stitched pixel dimensions.
    // If targetSize > 0, the result is resampled to targetSize x targetSize.
    static std::vector<uint8_t> generate(
        double minLat, double maxLat, double minLon, double maxLon,
        int zoom, int targetSize,
        const std::string& cacheDir,
        int& outputWidth, int& outputHeight,
        ProgressCallback progress = nullptr);

    // Write RGB data as PNG using stb_image_write
    static bool writePNG(const std::string& path,
                         const std::vector<uint8_t>& rgbData,
                         int width, int height);

    // Suggest a zoom level for the given area and target resolution.
    // Picks the zoom where tile pixels roughly match target pixel density.
    static int suggestZoom(double minLat, double maxLat,
                           double minLon, double maxLon,
                           int targetSize);

    // --- Progressive (two-pass) generation ---
    // Pass 1 returns a low-zoom preview immediately (blocking, fast).
    // Pass 2 runs in background at full zoom and calls onComplete when done.
    struct ProgressiveResult {
        std::vector<uint8_t> lowResData;   // Pass 1 RGB (available immediately)
        int lowResWidth = 0;
        int lowResHeight = 0;
    };

    // Completion callback receives the high-res data.
    using CompletionCallback = std::function<void(std::vector<uint8_t> highResData,
                                                   int width, int height)>;

    // Start progressive two-pass generation.
    // Returns low-res preview data. High-res generation runs on a background thread
    // and calls onComplete (from the background thread) when done.
    // previewZoomDelta: how many zoom levels below target for the preview (default 3).
    static ProgressiveResult generateProgressive(
        double minLat, double maxLat, double minLon, double maxLon,
        int zoom, int targetSize,
        const std::string& cacheDir,
        CompletionCallback onComplete,
        ProgressCallback progress = nullptr,
        int previewZoomDelta = 3);
};
