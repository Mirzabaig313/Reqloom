// CurlHttpClient — libcurl-backed HttpClient.
#include "CurlHttpClient.h"

#include <curl/curl.h>

#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>

namespace chainapi::engine {

namespace {

// ─── RAII wrappers ───────────────────────────────────────────────────────────

struct CurlEasyDeleter {
    void operator()(CURL* p) const noexcept {
        if (p) curl_easy_cleanup(p);
    }
};
using CurlEasyHandle = std::unique_ptr<CURL, CurlEasyDeleter>;

struct CurlSlistDeleter {
    void operator()(curl_slist* p) const noexcept {
        if (p) curl_slist_free_all(p);
    }
};
using CurlSlistHandle = std::unique_ptr<curl_slist, CurlSlistDeleter>;

struct CurlMimeDeleter {
    void operator()(curl_mime* p) const noexcept {
        if (p) curl_mime_free(p);
    }
};
using CurlMimeHandle = std::unique_ptr<curl_mime, CurlMimeDeleter>;

// ─── Process-wide curl global init ──────────────────────────────────────────

void ensureCurlGlobalInit() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        // Never call curl_global_cleanup — process exit reclaims everything,
        // and calling it from a destructor races with other live instances.
    });
}

// ─── Callbacks ───────────────────────────────────────────────────────────────

std::size_t writeCallback(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* body = static_cast<std::string*>(userdata);
    body->append(ptr, size * nmemb);
    return size * nmemb;
}

std::size_t headerCallback(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* headers = static_cast<std::vector<std::pair<std::string, std::string>>*>(userdata);
    std::string line(ptr, size * nmemb);
    auto colonPos = line.find(':');
    if (colonPos == std::string::npos) return size * nmemb;

    auto key = line.substr(0, colonPos);
    auto value = line.substr(colonPos + 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.erase(value.begin());
    }
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) {
        value.pop_back();
    }
    headers->emplace_back(std::move(key), std::move(value));
    return size * nmemb;
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

[[nodiscard]] ErrorCode classifyCurlError(CURLcode res) noexcept {
    switch (res) {
        case CURLE_COULDNT_RESOLVE_HOST:
            return ErrorCode::NetworkDns;
        case CURLE_SSL_CONNECT_ERROR:
        case CURLE_PEER_FAILED_VERIFICATION:
            return ErrorCode::NetworkTls;
        case CURLE_OPERATION_TIMEDOUT:
            return ErrorCode::NetworkTimeout;
        default:
            return ErrorCode::NetworkTimeout;
    }
}

}  // namespace

// ─── Public ──────────────────────────────────────────────────────────────────

CurlHttpClient::CurlHttpClient() {
    ensureCurlGlobalInit();
}

CurlHttpClient::~CurlHttpClient() = default;

std::expected<HttpResponse, ChainApiError> CurlHttpClient::send(const HttpRequest& request) {
    CurlEasyHandle curl{curl_easy_init()};
    if (!curl) {
        return std::unexpected(ChainApiError{
            ErrorCode::NetworkTimeout, ErrorClass::Network, "Failed to initialize curl handle"});
    }

    HttpResponse response;
    std::string responseBody;
    std::vector<std::pair<std::string, std::string>> responseHeaders;

    curl_easy_setopt(curl.get(), CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, methodString(request.method));
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &responseBody);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &responseHeaders);
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
    // chrono::milliseconds::rep is long on libstdc++ but long long on libc++,
    // so the cast is platform-conditional. Suppress GCC's useless-cast on the
    // platform where it happens to be a no-op.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wuseless-cast"
#endif
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, static_cast<long>(request.timeout.count()));
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

    // Connect timeout — configurable via TransportConfig::connectTimeout.
    // Default 5s matches the hard-coded value before this knob existed.
    curl_easy_setopt(curl.get(),
                     CURLOPT_CONNECTTIMEOUT_MS,
                     static_cast<long>(request.transport.connectTimeout.count()));
    // No-signal mode: libcurl normally uses SIGALRM for the resolver
    // timeout when built without c-ares. Disabling signals lets the
    // pure-millisecond connect timeout above actually take effect on
    // platforms where the SIGALRM handler isn't reliably installed.
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);

    // TLS verification. Both fields default to true; flipping either to
    // false yields curl's `--insecure` semantics for that layer. The
    // CAINFO path, when set, replaces the system trust store rather
    // than augmenting it — that's libcurl's contract.
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, request.transport.tlsVerify ? 1L : 0L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, request.transport.tlsVerifyHost ? 2L : 0L);
    if (request.transport.caBundlePath) {
        curl_easy_setopt(curl.get(), CURLOPT_CAINFO, request.transport.caBundlePath->c_str());
    }

    // Proxy. Empty leaves libcurl on its default (which honours the
    // HTTPS_PROXY / NO_PROXY env vars per request); a non-empty value
    // pins the request to that proxy regardless of env state.
    if (request.transport.proxy && !request.transport.proxy->empty()) {
        curl_easy_setopt(curl.get(), CURLOPT_PROXY, request.transport.proxy->c_str());
    }

    CurlSlistHandle headerList;
    for (const auto& [key, value] : request.headers) {
        // Multipart routing sets Content-Type itself (with the boundary),
        // so suppress any caller-supplied Content-Type for those requests.
        if (!request.multipart.empty() && key == "Content-Type") {
            continue;
        }
        auto headerLine = key + ": " + value;
        auto* appended = curl_slist_append(headerList.get(), headerLine.c_str());
        if (appended == nullptr) {
            return std::unexpected(ChainApiError{
                ErrorCode::NetworkTimeout, ErrorClass::Network, "Failed to append header: " + key});
        }
        // curl_slist_append returns the updated head — release the old and adopt the new.
        (void)headerList.release();
        headerList.reset(appended);
    }
    if (headerList) {
        curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headerList.get());
    }

    // Multipart and inline body are mutually exclusive. Multipart wins
    // when both are set; the upper layer guarantees only one is populated.
    CurlMimeHandle mime;
    if (!request.multipart.empty()) {
        mime.reset(curl_mime_init(curl.get()));
        if (!mime) {
            return std::unexpected(ChainApiError{ErrorCode::NetworkTimeout,
                                                 ErrorClass::Network,
                                                 "curl_mime_init failed (multipart request)"});
        }
        for (const auto& part : request.multipart) {
            curl_mimepart* mp = curl_mime_addpart(mime.get());
            if (mp == nullptr) {
                return std::unexpected(
                    ChainApiError{ErrorCode::NetworkTimeout,
                                  ErrorClass::Network,
                                  "curl_mime_addpart failed for part: " + part.name});
            }
            if (curl_mime_name(mp, part.name.c_str()) != CURLE_OK) {
                return std::unexpected(
                    ChainApiError{ErrorCode::NetworkTimeout,
                                  ErrorClass::Network,
                                  "curl_mime_name failed for part: " + part.name});
            }
            if (part.isFile()) {
                if (curl_mime_filedata(mp, part.filePath->c_str()) != CURLE_OK) {
                    return std::unexpected(
                        ChainApiError{ErrorCode::NetworkTimeout,
                                      ErrorClass::Network,
                                      "curl_mime_filedata failed: " + *part.filePath});
                }
                if (part.filename) {
                    curl_mime_filename(mp, part.filename->c_str());
                }
            } else {
                if (curl_mime_data(mp, part.value.c_str(), part.value.size()) != CURLE_OK) {
                    return std::unexpected(
                        ChainApiError{ErrorCode::NetworkTimeout,
                                      ErrorClass::Network,
                                      "curl_mime_data failed for part: " + part.name});
                }
            }
            if (part.contentType) {
                curl_mime_type(mp, part.contentType->c_str());
            }
        }
        curl_easy_setopt(curl.get(), CURLOPT_MIMEPOST, mime.get());
    } else if (request.body) {
        curl_easy_setopt(
            curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body->size()));
        curl_easy_setopt(curl.get(), CURLOPT_COPYPOSTFIELDS, request.body->c_str());
    }

    auto startTime = std::chrono::steady_clock::now();
    CURLcode res = curl_easy_perform(curl.get());
    auto elapsed = std::chrono::steady_clock::now() - startTime;

    if (res != CURLE_OK) {
        return std::unexpected(
            ChainApiError{classifyCurlError(res),
                          ErrorClass::Network,
                          std::string("curl error: ") + curl_easy_strerror(res)});
    }

    long httpStatus = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &httpStatus);

    response.status = static_cast<int>(httpStatus);
    response.body = std::move(responseBody);
    response.headers = std::move(responseHeaders);
    response.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
    return response;
}

}  // namespace chainapi::engine
