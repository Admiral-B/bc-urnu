#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

// Reuses a single WinHTTP session and per-domain connect handles across
// multiple download requests. Avoids the overhead of WinHttpOpen/Connect
// per tile, which was the main bottleneck in the old TileDownloader.
class WinHTTPConnectionPool {
public:
    explicit WinHTTPConnectionPool(const std::string& userAgent = "BridgeCommand/6.0");
    ~WinHTTPConnectionPool();

    WinHTTPConnectionPool(const WinHTTPConnectionPool&) = delete;
    WinHTTPConnectionPool& operator=(const WinHTTPConnectionPool&) = delete;

    // HTTP GET a URL. Thread-safe (connect handles are per-domain, serialized).
    // Returns response body bytes, or empty on failure.
    std::vector<uint8_t> get(const std::string& url);

    // HTTP POST. Thread-safe.
    std::vector<uint8_t> post(const std::string& url, const std::string& body,
                               const std::string& contentType = "application/x-www-form-urlencoded");

    void setUserAgent(const std::string& ua);

#ifdef _WIN32
    // Exposed for testing: get the session handle (one per pool lifetime).
    void* sessionHandle() const { return hSession; }
#endif

private:
#ifdef _WIN32
    struct DomainConnection {
        HINTERNET hConnect = nullptr;
        std::mutex mutex; // serialize requests to same domain
    };

    HINTERNET hSession = nullptr;
    std::mutex poolMutex;
    std::unordered_map<std::string, DomainConnection*> connections;

    DomainConnection* getConnection(const std::string& host, INTERNET_PORT port);
    std::vector<uint8_t> doRequest(const std::string& method, const std::string& url,
                                    const void* body, DWORD bodyLen,
                                    const std::string& contentType);

    struct ParsedURL {
        std::string host;
        std::string path;
        bool https = true;
        INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    };
    static bool parseURL(const std::string& url, ParsedURL& out);
#endif
};
