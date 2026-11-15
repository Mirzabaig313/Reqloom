// Engine-internal HTTP client interface. Concrete implementation:
// CurlHttpClient. No Qt types leak.
#pragma once

#include <reqloom/engine/ErrorCodes.h>
#include <reqloom/engine/Operation.h>
#include <reqloom/engine/Transport.h>

#include <chrono>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace reqloom::engine {

/// One part of a multipart/form-data request body.
///

struct MultipartPart {
    std::string name;
    std::string value;
    std::optional<std::string> filePath;     ///< diagnostic; presence implies file part
    std::optional<std::string> filename;     ///< explicit filename override
    std::optional<std::string> contentType;  ///< MIME type override

    [[nodiscard]] bool isFile() const noexcept { return filePath.has_value(); }
};

/// Per-send transport configuration, stamped by the executor from the active environment. Aliases
/// the public `reqloom::engine::TransportConfig`.
using TransportConfig = ::reqloom::engine::TransportConfig;

struct HttpRequest {
    HttpMethod method{HttpMethod::Get};
    std::string url;
    std::map<std::string, std::string> headers;
    std::optional<std::string> body;

    /// When non-empty the request is sent as `multipart/form-data` and `body` is ignored; the
    /// Content-Type is set automatically and any caller-set one is overwritten.
    std::vector<MultipartPart> multipart;

    std::chrono::milliseconds timeout{30'000};

    /// Per-send transport overrides; defaults are safe (TLS verified, no proxy, 5s connect
    /// timeout).
    TransportConfig transport;

    /// When false a 3xx response is returned as is instead of being followed.
    bool followRedirects{true};
};

struct HttpResponse {
    int status{0};
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    std::chrono::milliseconds elapsed{};
};

class HttpClient {
public:
    HttpClient() = default;
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;
    HttpClient(HttpClient&&) = delete;
    HttpClient& operator=(HttpClient&&) = delete;
    virtual ~HttpClient() = default;

    /// Synchronous. Network failures are `ReqloomError` (NetworkTimeout / NetworkDns / NetworkTls);
    /// HTTP status codes are the caller's concern.
    virtual std::expected<HttpResponse, ReqloomError> send(const HttpRequest& request) = 0;
};

}  // namespace reqloom::engine
