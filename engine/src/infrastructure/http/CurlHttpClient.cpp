// CurlHttpClient — libcurl-backed HttpClient.
#include "CurlHttpClient.h"

#include <reqloom/engine/HttpDefaults.h>

#include <curl/curl.h>

#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace reqloom::engine {

namespace {

/// Case-insensitive ASCII header-name comparison, so a caller's "user-agent" overrides our default.
[[nodiscard]] bool headerNameEquals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// Clamp a 64-bit count into libcurl's `long` timeout slots (32-bit on Windows, where a cast
// truncates).
[[nodiscard]] long toCurlLongClamped(std::int64_t v) noexcept {
    // Parenthesised so <windows.h>'s min/max macros do not expand.
    constexpr auto kMax = (std::numeric_limits<long>::max)();
    constexpr auto kMin = (std::numeric_limits<long>::min)();
    if (v > static_cast<std::int64_t>(kMax)) {
        return kMax;
    }
    if (v < static_cast<std::int64_t>(kMin)) {
        return kMin;
    }
    return static_cast<long>(v);
}

struct CurlEasyDeleter {
    void operator()(CURL* p) const noexcept {
        if (p != nullptr) {
            curl_easy_cleanup(p);
        }
    }
};
using CurlEasyHandle = std::unique_ptr<CURL, CurlEasyDeleter>;

struct CurlSlistDeleter {
    void operator()(curl_slist* p) const noexcept {
        if (p != nullptr) {
            curl_slist_free_all(p);
        }
    }
};
using CurlSlistHandle = std::unique_ptr<curl_slist, CurlSlistDeleter>;

struct CurlMimeDeleter {
    void operator()(curl_mime* p) const noexcept {
        if (p != nullptr) {
            curl_mime_free(p);
        }
    }
};
using CurlMimeHandle = std::unique_ptr<curl_mime, CurlMimeDeleter>;

void ensureCurlGlobalInit() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        // Never call curl_global_cleanup — process exit reclaims everything,
        // and calling it from a destructor races with other live instances.
    });
}

// libcurl calls these from C frames, so they are noexcept and signal failure with a short count
// (CURLE_WRITE_ERROR). Response body accumulator with a hard cap on the decompressed size.
struct BodySink {
    std::string body;
    std::size_t limit{0};
    bool limitExceeded{false};
};

std::size_t writeCallback(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) noexcept {
    const std::size_t total = size * nmemb;
    auto* sink = static_cast<BodySink*>(userdata);
    // body.size() never exceeds limit, so the subtraction cannot wrap.
    if (total > sink->limit - sink->body.size()) {
        sink->limitExceeded = true;
        return 0;
    }
    try {
        sink->body.append(ptr, total);
    } catch (...) {
        return 0;
    }
    return total;
}

std::size_t headerCallback(char* ptr,
                           std::size_t size,
                           std::size_t nmemb,
                           void* userdata) noexcept {
    const std::size_t total = size * nmemb;
    auto* headers = static_cast<std::vector<std::pair<std::string, std::string>>*>(userdata);
    try {
        std::string const line(ptr, total);
        // A status line starts each response (redirects, 100 Continue, CONNECT); keep only the
        // final response's headers.
        if (line.starts_with("HTTP/")) {
            headers->clear();
            return total;
        }
        auto colonPos = line.find(':');
        if (colonPos == std::string::npos) {
            return total;
        }

        auto key = line.substr(0, colonPos);
        auto value = line.substr(colonPos + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
            value.erase(value.begin());
        }
        while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) {
            value.pop_back();
        }
        headers->emplace_back(std::move(key), std::move(value));
    } catch (...) {
        return 0;
    }
    return total;
}

[[nodiscard]] const char* methodString(HttpMethod method) noexcept {
    switch (method) {
        case HttpMethod::Get:
            return "GET";
        case HttpMethod::Post:
            return "POST";
        case HttpMethod::Put:
            return "PUT";
        case HttpMethod::Patch:
            return "PATCH";
        case HttpMethod::Delete:
            return "DELETE";
        case HttpMethod::Head:
            return "HEAD";
        case HttpMethod::Options:
            return "OPTIONS";
    }
    return "GET";
}

// `requestSent` is true when request bytes may already have reached a server (an earlier
// redirect hop, or a non-zero CURLINFO_REQUEST_SIZE).
[[nodiscard]] ErrorCode classifyCurlError(CURLcode res, bool requestSent) noexcept {
    switch (res) {
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_RESOLVE_PROXY:
            return requestSent ? ErrorCode::NetworkTimeout : ErrorCode::NetworkDns;
        case CURLE_COULDNT_CONNECT:
            // Nothing reached the origin, unless an earlier redirect hop already delivered the
            // request.
            return requestSent ? ErrorCode::NetworkTimeout : ErrorCode::NetworkConnect;
        case CURLE_SSL_CONNECT_ERROR:
        case CURLE_PEER_FAILED_VERIFICATION:
            return ErrorCode::NetworkTls;
        // Every other libcurl error (timeouts, send/receive errors, empty reply) maps to
        // NetworkTimeout: the request may have reached the server.
        default:
            return ErrorCode::NetworkTimeout;
    }
}

constexpr long kMaxRedirects = 10;

[[nodiscard]] ReqloomError describeCurlFailure(CURLcode res,
                                               const BodySink& sink,
                                               bool requestSent) {
    const std::string curlText = curl_easy_strerror(res);
    if (res == CURLE_WRITE_ERROR && sink.limitExceeded) {
        return ReqloomError{ErrorCode::NetworkPolicy,
                            ErrorClass::Network,
                            std::format("response body exceeded the limit of {} bytes "
                                        "(transport.maxResponseBytes); transfer aborted",
                                        sink.limit)};
    }
    if (res == CURLE_UNSUPPORTED_PROTOCOL) {
        return ReqloomError{
            ErrorCode::NetworkPolicy,
            ErrorClass::Network,
            "URL scheme not allowed: only http:// and https:// are supported (curl: " + curlText +
                ")"};
    }
    if (res == CURLE_TOO_MANY_REDIRECTS) {
        return ReqloomError{
            ErrorCode::NetworkPolicy,
            ErrorClass::Network,
            std::format(
                "too many redirects: stopped after {} (curl: {})", kMaxRedirects, curlText)};
    }
    return ReqloomError{
        classifyCurlError(res, requestSent), ErrorClass::Network, "curl error: " + curlText};
}

}  // namespace

CurlHttpClient::CurlHttpClient() {
    ensureCurlGlobalInit();
}

CurlHttpClient::~CurlHttpClient() = default;

std::expected<HttpResponse, ReqloomError> CurlHttpClient::send(const HttpRequest& request) {
    CurlEasyHandle const curl{curl_easy_init()};
    if (!curl) {
        return std::unexpected(ReqloomError{
            ErrorCode::NetworkTimeout, ErrorClass::Network, "Failed to initialize curl handle"});
    }

    HttpResponse response;
    BodySink responseBody;
    responseBody.limit = request.transport.maxResponseBytes;
    std::vector<std::pair<std::string, std::string>> responseHeaders;

    curl_easy_setopt(curl.get(), CURLOPT_URL, request.url.c_str());
    // HEAD goes through CURLOPT_NOBODY: as a custom request libcurl waits for a body that never
    // arrives.
    if (request.method == HttpMethod::Head) {
        curl_easy_setopt(curl.get(), CURLOPT_NOBODY, 1L);
    } else {
        curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, methodString(request.method));
    }
    // Only http(s), for every redirect hop too: no file://, gopher:// etc. from a project URL or a
    // Location header.
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl.get(), CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &responseBody);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &responseHeaders);
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, request.followRedirects ? 1L : 0L);
    curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, kMaxRedirects);
    curl_easy_setopt(curl.get(),
                     CURLOPT_TIMEOUT_MS,
                     toCurlLongClamped(static_cast<std::int64_t>(request.timeout.count())));

    curl_easy_setopt(
        curl.get(),
        CURLOPT_CONNECTTIMEOUT_MS,
        toCurlLongClamped(static_cast<std::int64_t>(request.transport.connectTimeout.count())));

    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, request.transport.tlsVerify ? 1L : 0L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, request.transport.tlsVerifyHost ? 2L : 0L);
    if (request.transport.caBundlePath) {
        curl_easy_setopt(curl.get(), CURLOPT_CAINFO, request.transport.caBundlePath->c_str());
    }

    // Mutual TLS: PEM is assumed (libcurl's default SSLCERTTYPE); an encrypted key uses the
    // passphrase.
    if (request.transport.clientCertPath && !request.transport.clientCertPath->empty()) {
        curl_easy_setopt(curl.get(), CURLOPT_SSLCERT, request.transport.clientCertPath->c_str());
    }
    if (request.transport.clientCertType && !request.transport.clientCertType->empty()) {
        curl_easy_setopt(
            curl.get(), CURLOPT_SSLCERTTYPE, request.transport.clientCertType->c_str());
    }
    if (request.transport.clientKeyPath && !request.transport.clientKeyPath->empty()) {
        curl_easy_setopt(curl.get(), CURLOPT_SSLKEY, request.transport.clientKeyPath->c_str());
    }
    if (request.transport.clientKeyPassword && !request.transport.clientKeyPassword->empty()) {
        curl_easy_setopt(
            curl.get(), CURLOPT_KEYPASSWD, request.transport.clientKeyPassword->c_str());
    }

    // Empty keeps libcurl's default (HTTPS_PROXY / NO_PROXY env vars); non-empty pins the proxy.
    if (request.transport.proxy && !request.transport.proxy->empty()) {
        curl_easy_setopt(curl.get(), CURLOPT_PROXY, request.transport.proxy->c_str());
    }

    CurlSlistHandle headerList;
    // The released pointer is the head `appended` already references, so discarding it is correct.
    auto appendHeaderLine = [&headerList](const std::string& line) -> bool {
        auto* appended = curl_slist_append(headerList.get(), line.c_str());
        if (appended == nullptr) {
            return false;
        }
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        (void)headerList.release();
        headerList.reset(appended);
        return true;
    };

    // Track which default headers the caller pinned, so a user-supplied value always wins.
    bool hasAcceptEncoding{false};
    bool hasConnection{false};
    for (const auto& [key, value] : request.headers) {
        // Multipart sets its own Content-Type (with the boundary); drop any caller one,
        // case-insensitively.
        if (!request.multipart.empty() && headerNameEquals(key, "Content-Type")) {
            continue;
        }
        if (headerNameEquals(key, "Accept-Encoding")) {
            hasAcceptEncoding = true;
        } else if (headerNameEquals(key, "Connection")) {
            hasConnection = true;
        }
        if (!appendHeaderLine(std::format("{}: {}", key, value))) {
            return std::unexpected(ReqloomError{
                ErrorCode::NetworkTimeout, ErrorClass::Network, "Failed to append header: " + key});
        }
    }

    // Auto-generated headers (mirrored in the UI panel); Connection only when the caller set none.
    if (!hasConnection && !appendHeaderLine(std::format("Connection: {}", kDefaultConnection))) {
        return std::unexpected(ReqloomError{ErrorCode::NetworkTimeout,
                                            ErrorClass::Network,
                                            "Failed to append default Connection header"});
    }
    if (headerList) {
        curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headerList.get());
    }

    // libcurl sends no User-Agent by default; a caller's header in the list overrides this one.
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, std::string(kDefaultUserAgent).c_str());
    // Advertise gzip/deflate for libcurl to decompress, unless the caller pinned an encoding.
    if (!hasAcceptEncoding) {
        curl_easy_setopt(
            curl.get(), CURLOPT_ACCEPT_ENCODING, std::string(kDefaultAcceptEncoding).c_str());
    }

    // Multipart wins over an inline body; the upper layer populates only one.
    CurlMimeHandle mime;
    if (!request.multipart.empty()) {
        mime.reset(curl_mime_init(curl.get()));
        if (!mime) {
            return std::unexpected(ReqloomError{ErrorCode::NetworkTimeout,
                                                ErrorClass::Network,
                                                "curl_mime_init failed (multipart request)"});
        }
        for (const auto& part : request.multipart) {
            curl_mimepart* mp = curl_mime_addpart(mime.get());
            if (mp == nullptr) {
                return std::unexpected(
                    ReqloomError{ErrorCode::NetworkTimeout,
                                 ErrorClass::Network,
                                 "curl_mime_addpart failed for part: " + part.name});
            }
            if (curl_mime_name(mp, part.name.c_str()) != CURLE_OK) {
                return std::unexpected(
                    ReqloomError{ErrorCode::NetworkTimeout,
                                 ErrorClass::Network,
                                 "curl_mime_name failed for part: " + part.name});
            }
            // File parts were pre-loaded by MultipartBuilder, so libcurl never re-opens the path
            // (TOCTOU).
            if (curl_mime_data(mp, part.value.data(), part.value.size()) != CURLE_OK) {
                return std::unexpected(
                    ReqloomError{ErrorCode::NetworkTimeout,
                                 ErrorClass::Network,
                                 "curl_mime_data failed for part: " + part.name});
            }
            if (part.isFile() && part.filename) {
                curl_mime_filename(mp, part.filename->c_str());
            }
            if (part.contentType) {
                curl_mime_type(mp, part.contentType->c_str());
            }
        }
        curl_easy_setopt(curl.get(), CURLOPT_MIMEPOST, mime.get());
    } else if (request.body) {
        // _LARGE takes curl_off_t; POSTFIELDSIZE is `long` and truncates past 2 GiB on Windows.
        curl_easy_setopt(
            curl.get(), CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body->size()));
        // POSTFIELDS, not COPYPOSTFIELDS: `request` outlives the synchronous perform. Set the size
        // first so the buffer is opaque bytes (may contain NULs).
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, request.body->data());
    }

    auto startTime = std::chrono::steady_clock::now();
    CURLcode const res = curl_easy_perform(curl.get());
    auto elapsed = std::chrono::steady_clock::now() - startTime;

    if (res != CURLE_OK) {
        long redirects = 0;
        long requestBytes = 0;
        curl_easy_getinfo(curl.get(), CURLINFO_REDIRECT_COUNT, &redirects);
        curl_easy_getinfo(curl.get(), CURLINFO_REQUEST_SIZE, &requestBytes);
        return std::unexpected(
            describeCurlFailure(res, responseBody, redirects > 0 || requestBytes > 0));
    }

    long httpStatus = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &httpStatus);

    response.status = static_cast<int>(httpStatus);
    response.body = std::move(responseBody.body);
    response.headers = std::move(responseHeaders);
    response.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
    return response;
}

}  // namespace reqloom::engine
