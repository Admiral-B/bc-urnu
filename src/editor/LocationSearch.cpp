#include "LocationSearch.hpp"
#include "../libs/nlohmann/json.hpp"

#include <sstream>
#include <cmath>
#include <cctype>
#include <thread>
#include <algorithm>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#endif

using json = nlohmann::json;

// ---- Coordinate parsing ----

bool LocationSearch::parseCoordinates(const std::string& input, double& lat, double& lon) {
    // Try decimal format first: "51.5, -0.1" or "51.5 -0.1"
    std::string cleaned = input;
    // Replace common separators
    for (char& c : cleaned) {
        if (c == ';') c = ',';
    }

    // Try "lat, lon" format
    size_t commaPos = cleaned.find(',');
    if (commaPos != std::string::npos) {
        try {
            lat = std::stod(cleaned.substr(0, commaPos));
            lon = std::stod(cleaned.substr(commaPos + 1));
            if (lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0)
                return true;
        } catch (...) {}
    }

    // Try space-separated "lat lon"
    std::istringstream iss(cleaned);
    double a, b;
    if (iss >> a >> b) {
        if (a >= -90.0 && a <= 90.0 && b >= -180.0 && b <= 180.0) {
            lat = a;
            lon = b;
            return true;
        }
    }

    // Try DMS format
    if (parseDMS(input, lat, lon))
        return true;

    return false;
}

bool LocationSearch::parseDMS(const std::string& input, double& lat, double& lon) {
    // Parse formats like:
    //   51 30'N 0 06'W
    //   51°30'N 0°06'W
    //   51°30'00"N 0°06'00"W
    //   N51 30 W0 06
    std::string s = input;

    // Normalize degree symbols
    // UTF-8 degree sign is 0xC2 0xB0
    std::string result;
    for (size_t i = 0; i < s.size(); i++) {
        if (i + 1 < s.size() && (unsigned char)s[i] == 0xC2 && (unsigned char)s[i+1] == 0xB0) {
            result += ' ';
            i++; // skip second byte
        } else if (s[i] == '\'' || s[i] == '"') {
            result += ' ';
        } else {
            result += s[i];
        }
    }
    s = result;

    // Convert to uppercase for direction parsing
    std::string upper = s;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);

    // Find N/S and E/W markers
    bool hasN = upper.find('N') != std::string::npos;
    bool hasS = upper.find('S') != std::string::npos;
    bool hasE = upper.find('E') != std::string::npos;
    bool hasW = upper.find('W') != std::string::npos;

    if (!(hasN || hasS) || !(hasE || hasW))
        return false;

    // Split at direction letters
    // Extract numbers before/after N/S and E/W
    auto extractNumbers = [](const std::string& str, size_t start, size_t end) -> std::vector<double> {
        std::vector<double> nums;
        std::string sub = str.substr(start, end - start);
        std::istringstream iss(sub);
        double val;
        while (iss >> val) {
            nums.push_back(val);
        }
        return nums;
    };

    // Find positions of direction letters
    size_t latDirPos = std::string::npos;
    size_t lonDirPos = std::string::npos;
    for (size_t i = 0; i < upper.size(); i++) {
        if ((upper[i] == 'N' || upper[i] == 'S') && latDirPos == std::string::npos)
            latDirPos = i;
        if ((upper[i] == 'E' || upper[i] == 'W') && lonDirPos == std::string::npos)
            lonDirPos = i;
    }

    if (latDirPos == std::string::npos || lonDirPos == std::string::npos)
        return false;

    // Determine which comes first
    std::vector<double> latNums, lonNums;
    bool latNeg = hasS;
    bool lonNeg = hasW;

    if (latDirPos < lonDirPos) {
        // Format: ...N/S...E/W
        latNums = extractNumbers(s, 0, latDirPos);
        lonNums = extractNumbers(s, latDirPos + 1, lonDirPos);
    } else {
        // Format: ...E/W...N/S (less common)
        lonNums = extractNumbers(s, 0, lonDirPos);
        latNums = extractNumbers(s, lonDirPos + 1, latDirPos);
    }

    if (latNums.empty() || lonNums.empty())
        return false;

    // Convert DMS to decimal
    double latVal = latNums[0];
    if (latNums.size() > 1) latVal += latNums[1] / 60.0;
    if (latNums.size() > 2) latVal += latNums[2] / 3600.0;

    double lonVal = lonNums[0];
    if (lonNums.size() > 1) lonVal += lonNums[1] / 60.0;
    if (lonNums.size() > 2) lonVal += lonNums[2] / 3600.0;

    if (latNeg) latVal = -latVal;
    if (lonNeg) lonVal = -lonVal;

    if (latVal >= -90.0 && latVal <= 90.0 && lonVal >= -180.0 && lonVal <= 180.0) {
        lat = latVal;
        lon = lonVal;
        return true;
    }

    return false;
}

// ---- HTTP helper ----

#ifdef _WIN32
std::vector<uint8_t> LocationSearch::httpGet(const std::string& url, const std::string& userAgent) {
    std::vector<uint8_t> result;

    // Parse URL
    std::string host, path;
    bool useHttps = true;
    size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) return result;

    std::string scheme = url.substr(0, schemeEnd);
    if (scheme == "http") useHttps = false;

    size_t hostStart = schemeEnd + 3;
    size_t pathStart = url.find('/', hostStart);
    if (pathStart == std::string::npos) {
        host = url.substr(hostStart);
        path = "/";
    } else {
        host = url.substr(hostStart, pathStart - hostStart);
        path = url.substr(pathStart);
    }

    std::wstring wHost(host.begin(), host.end());
    std::wstring wPath(path.begin(), path.end());
    std::wstring wUA(userAgent.begin(), userAgent.end());

    HINTERNET hSession = WinHttpOpen(wUA.c_str(),
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return result;

    INTERNET_PORT port = useHttps ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    HINTERNET hConnect = WinHttpConnect(hSession, wHost.c_str(), port, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return result;
    }

    DWORD flags = useHttps ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", wPath.c_str(),
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return result;
    }

    DWORD timeout = 10000;
    WinHttpSetTimeouts(hRequest, timeout, timeout, timeout, timeout);

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(hRequest, NULL)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return result;
    }

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize,
        WINHTTP_NO_HEADER_INDEX);

    if (statusCode != 200) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return result;
    }

    DWORD bytesAvailable = 0;
    DWORD bytesRead = 0;
    do {
        if (!WinHttpQueryDataAvailable(hRequest, &bytesAvailable)) break;
        if (bytesAvailable == 0) break;
        std::vector<uint8_t> buffer(bytesAvailable);
        if (!WinHttpReadData(hRequest, buffer.data(), bytesAvailable, &bytesRead)) break;
        result.insert(result.end(), buffer.begin(), buffer.begin() + bytesRead);
    } while (bytesAvailable > 0);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return result;
}
#else
std::vector<uint8_t> LocationSearch::httpGet(const std::string& /*url*/, const std::string& /*userAgent*/) {
    return {}; // Nominatim search not implemented on non-Windows yet
}
#endif

// ---- Nominatim search ----

std::vector<LocationSearch::Result> LocationSearch::searchPlaceName(const std::string& query) {
    std::vector<Result> results;

    if (query.empty())
        return results;

    // Rate limit: 1 request per second
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastRequestTime);
    if (elapsed.count() < 1000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000 - elapsed.count()));
    }
    lastRequestTime = std::chrono::steady_clock::now();

    // URL-encode the query
    std::string encoded;
    for (char c : query) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += c;
        } else if (c == ' ') {
            encoded += '+';
        } else {
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", (unsigned char)c);
            encoded += hex;
        }
    }

    std::string url = "https://nominatim.openstreetmap.org/search?q=" + encoded +
                      "&format=json&limit=5";

    auto data = httpGet(url, "BridgeCommand/6.0 (scenario-editor)");
    if (data.empty())
        return results;

    try {
        std::string jsonStr(data.begin(), data.end());
        json arr = json::parse(jsonStr);

        if (!arr.is_array())
            return results;

        for (const auto& item : arr) {
            Result r;
            if (item.contains("lat") && item.contains("lon")) {
                r.lat = std::stod(item["lat"].get<std::string>());
                r.lon = std::stod(item["lon"].get<std::string>());
            } else {
                continue;
            }
            if (item.contains("display_name") && item["display_name"].is_string()) {
                r.displayName = item["display_name"].get<std::string>();
                // Truncate long display names
                if (r.displayName.size() > 80)
                    r.displayName = r.displayName.substr(0, 77) + "...";
            }
            results.push_back(std::move(r));
        }
    } catch (...) {}

    return results;
}
