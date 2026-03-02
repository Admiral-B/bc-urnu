#pragma once

#include <string>
#include <vector>
#include <cstdint>

// Discovers and downloads free S-57 ENC charts from NOAA.
// Uses the NOAA Interactive Catalog JSON index to find charts by bounding box,
// then downloads chart ZIPs and extracts .000 files.
class ENCCatalog {
public:
    struct ChartInfo {
        std::string id;         // e.g. "US5WA18M"
        std::string title;      // e.g. "Puget Sound - Swinomish Channel"
        int scale = 0;          // e.g. 22000 (from "1:22,000")
        double minLat = 0, maxLat = 0, minLon = 0, maxLon = 0;
        std::string downloadUrl;
        bool isDownloaded = false;
    };

    // Download and parse the NOAA catalog JSON.
    // Caches the JSON locally for 24 hours in cacheDir.
    bool loadCatalog(const std::string& cacheDir);

    // Find all charts whose bounding box intersects the given area.
    // Returns charts sorted by scale (most detailed first, lowest scale number).
    std::vector<ChartInfo> findChartsForArea(
        double minLat, double maxLat, double minLon, double maxLon) const;

    // Download a chart ZIP, extract the .000 file to chartDir.
    // Returns path to the extracted .000 file, or empty on failure.
    std::string downloadChart(const ChartInfo& chart,
                              const std::string& chartDir);

    // Check if a chart's .000 file already exists locally.
    bool isChartCached(const ChartInfo& chart,
                       const std::string& chartDir) const;

    // How many charts in the catalog?
    size_t size() const { return catalog.size(); }

    // Is catalog loaded?
    bool isLoaded() const { return !catalog.empty(); }

private:
    std::vector<ChartInfo> catalog;
    std::string catalogCachePath;

    // HTTP download (reuses TileDownloader pattern)
    static std::vector<uint8_t> httpDownload(const std::string& url,
                                              const std::string& userAgent);

    // Parse NOAA enc.json catalog
    bool parseCatalogJSON(const std::string& jsonStr);

    // Parse scale string like "1:22,000" into integer 22000
    static int parseScale(const std::string& scaleStr);

    // Extract .000 file from a chart ZIP
    static std::string extractChartZip(const std::string& zipPath,
                                        const std::string& outputDir);
};
