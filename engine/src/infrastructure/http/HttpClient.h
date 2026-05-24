// Engine-internal HTTP client interface. Concrete implementation:
// CurlHttpClient. The interface is process-local; no Qt types leak.
#pragma once

#include <chainapi/engine/ErrorCodes.h>
#include <chainapi/engine/Operation.h>

#include <chrono>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace chainapi::engine {

struct HttpRequest {
    HttpMethod method{HttpMethod::Get};
    std::string url;
    std::map<std::string, std::string> headers;
    std::optional<std::string> body;
    std::chrono::milliseconds timeout{30'000};
};

struct HttpResponse {
    int status{0};
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    std::chrono::milliseconds elapsed{};
};

class HttpClient {
public:
    virtual ~HttpClient() = default;

    /// Synchronous; cancellation is via the engine-level cancel() that
    /// aborts the underlying transport (libcurl multi-handle).
    /// Engine Req §3.8. Network failures are surfaced as `ChainApiError`
    /// (NetworkTimeout / NetworkDns / NetworkTls); HTTP status codes are
    /// not — those are the caller's concern.
    virtual std::expected<HttpResponse, ChainApiError> send(
        const HttpRequest& request) = 0;
};

}  // namespace chainapi::engine
