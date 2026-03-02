#pragma once

#include <string>
#include <vector>
#include <chrono>

// Parses coordinate strings and geocodes place names via Nominatim.
class LocationSearch {
public:
    struct Result {
        double lat = 0.0;
        double lon = 0.0;
        std::string displayName;
    };

    // Try to parse a coordinate string (e.g. "51.5, -0.1" or "51 30'N 0 06'W").
    // Returns true if successfully parsed, fills lat/lon.
    static bool parseCoordinates(const std::string& input, double& lat, double& lon);

    // Search Nominatim for a place name. Returns up to 5 results.
    // Respects 1 request/second rate limit.
    std::vector<Result> searchPlaceName(const std::string& query);

    // Check if a search is currently in progress (for async future use)
    bool isBusy() const { return false; }

private:
    std::chrono::steady_clock::time_point lastRequestTime;

    static bool parseDMS(const std::string& input, double& lat, double& lon);
    static std::vector<uint8_t> httpGet(const std::string& url, const std::string& userAgent);
};
