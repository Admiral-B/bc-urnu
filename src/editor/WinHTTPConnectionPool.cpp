#include "WinHTTPConnectionPool.hpp"

#ifdef _WIN32
#pragma comment(lib, "winhttp.lib")
#endif

WinHTTPConnectionPool::WinHTTPConnectionPool(const std::string& userAgent) {
#ifdef _WIN32
    std::wstring wUA(userAgent.begin(), userAgent.end());
    hSession = WinHttpOpen(wUA.c_str(),
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
#endif
}

WinHTTPConnectionPool::~WinHTTPConnectionPool() {
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(poolMutex);
    for (auto& [key, conn] : connections) {
        if (conn->hConnect) WinHttpCloseHandle(conn->hConnect);
        delete conn;
    }
    connections.clear();
    if (hSession) WinHttpCloseHandle(hSession);
#endif
}

void WinHTTPConnectionPool::setUserAgent(const std::string& ua) {
    (void)ua;
    // User agent is set at session creation time; changing after creation
    // is not supported by WinHTTP without recreating the session.
}

std::vector<uint8_t> WinHTTPConnectionPool::get(const std::string& url) {
#ifdef _WIN32
    return doRequest("GET", url, nullptr, 0, "");
#else
    (void)url;
    return {};
#endif
}

std::vector<uint8_t> WinHTTPConnectionPool::post(const std::string& url,
                                                   const std::string& body,
                                                   const std::string& contentType) {
#ifdef _WIN32
    return doRequest("POST", url, body.c_str(), (DWORD)body.size(), contentType);
#else
    (void)url; (void)body; (void)contentType;
    return {};
#endif
}

#ifdef _WIN32

bool WinHTTPConnectionPool::parseURL(const std::string& url, ParsedURL& out) {
    size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) return false;

    std::string scheme = url.substr(0, schemeEnd);
    out.https = (scheme != "http");
    out.port = out.https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;

    size_t hostStart = schemeEnd + 3;
    size_t pathStart = url.find('/', hostStart);
    if (pathStart == std::string::npos) {
        out.host = url.substr(hostStart);
        out.path = "/";
    } else {
        out.host = url.substr(hostStart, pathStart - hostStart);
        out.path = url.substr(pathStart);
    }

    // Handle explicit port in host
    size_t colonPos = out.host.find(':');
    if (colonPos != std::string::npos) {
        out.port = (INTERNET_PORT)std::stoi(out.host.substr(colonPos + 1));
        out.host = out.host.substr(0, colonPos);
    }

    return true;
}

WinHTTPConnectionPool::DomainConnection*
WinHTTPConnectionPool::getConnection(const std::string& host, INTERNET_PORT port) {
    std::string key = host + ":" + std::to_string(port);

    std::lock_guard<std::mutex> lock(poolMutex);
    auto it = connections.find(key);
    if (it != connections.end()) return it->second;

    // Create new connect handle for this domain
    std::wstring wHost(host.begin(), host.end());
    HINTERNET hConnect = WinHttpConnect(hSession, wHost.c_str(), port, 0);
    if (!hConnect) return nullptr;

    auto* conn = new DomainConnection();
    conn->hConnect = hConnect;
    connections[key] = conn;
    return conn;
}

std::vector<uint8_t> WinHTTPConnectionPool::doRequest(
    const std::string& method, const std::string& url,
    const void* body, DWORD bodyLen, const std::string& contentType) {

    std::vector<uint8_t> result;
    if (!hSession) return result;

    ParsedURL parsed;
    if (!parseURL(url, parsed)) return result;

    DomainConnection* conn = getConnection(parsed.host, parsed.port);
    if (!conn) return result;

    // Serialize requests to the same domain's connect handle
    std::lock_guard<std::mutex> lock(conn->mutex);

    std::wstring wPath(parsed.path.begin(), parsed.path.end());
    std::wstring wMethod(method.begin(), method.end());
    DWORD flags = parsed.https ? WINHTTP_FLAG_SECURE : 0;

    HINTERNET hRequest = WinHttpOpenRequest(conn->hConnect, wMethod.c_str(),
        wPath.c_str(), NULL, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) return result;

    // Set timeout to 10 seconds
    DWORD timeout = 10000;
    WinHttpSetTimeouts(hRequest, timeout, timeout, timeout, timeout);

    // Add content-type header for POST
    LPCWSTR headers = WINHTTP_NO_ADDITIONAL_HEADERS;
    DWORD headersLen = 0;
    std::wstring wHeaders;
    if (!contentType.empty() && bodyLen > 0) {
        wHeaders = L"Content-Type: " + std::wstring(contentType.begin(), contentType.end());
        headers = wHeaders.c_str();
        headersLen = (DWORD)wHeaders.size();
    }

    BOOL sent = WinHttpSendRequest(hRequest, headers, headersLen,
        bodyLen > 0 ? const_cast<void*>(body) : WINHTTP_NO_REQUEST_DATA,
        bodyLen, bodyLen, 0);
    if (!sent) {
        WinHttpCloseHandle(hRequest);
        return result;
    }

    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        WinHttpCloseHandle(hRequest);
        return result;
    }

    // Check status code
    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize,
        WINHTTP_NO_HEADER_INDEX);

    if (statusCode != 200) {
        WinHttpCloseHandle(hRequest);
        return result;
    }

    // Read response
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
    return result;
}

#endif // _WIN32
