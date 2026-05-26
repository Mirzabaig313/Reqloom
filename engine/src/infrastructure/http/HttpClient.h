// Engine-internal HTTP client interface. Concrete implementation:
// CurlHttpClient. No Qt types leak.
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

/// One part of a multipart/form-data request body.
///
/// `value` carries the in-memory bytes for text fields (name = "qty",
/// value = "5"). `filePath` carries an absolute filesystem path for file
/// parts. When `filePath` is set, libcurl reads the file directly via
/// `curl_mime_filedata` — we never pre-load file bytes into memory.
struct MultipartPart {
    std::string name;
    std::string value;
    std::optional<std::string> filePath;
    std::optional<std::string> filename;     ///< explicit filename override
    std::optional<std::string> contentType;  ///< MIME type override

    [[nodiscard]] bool isFile() const noexcept { return filePath.has_value(); }
};

struct HttpRequest {
    HttpMethod method{HttpMethod::Get};
    std::string url;
    std::map<std::string, std::string> headers;
    std::optional<std::string> body;

    /// When non-empty, the request is sent as `multipart/form-data` and
    /// `body` is ignored. The Content-Type header is set automatically
    /// (with the boundary libcurl picks); any caller-set Content-Type
    /// is overwritten.
    std::vector<MultipartPart> multipart;

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

    /// Synchronous. Network failures are surfaced as `ChainApiError`
    /// (NetworkTimeout / NetworkDns / NetworkTls); HTTP status codes are
    /// not — those are the caller's concern.
    virtual std::expected<HttpResponse, ChainApiError> send(const HttpRequest& request) = 0;
};

}  // namespace chainapi::engine
