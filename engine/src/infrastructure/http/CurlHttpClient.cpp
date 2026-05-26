// CurlHttpClient — libcurl-backed implementation of HttpClient.
//
// Design notes:
//   - Curl handle and slist are RAII-wrapped.
//   - Global init is process-wide (libcurl requires once per process).
//     Multiple instances share one global init.
//   - CURLOPT_COPYPOSTFIELDS decouples the body buffer lifetime from the request.
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
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, static_cast<long>(request.timeout.count()));
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 5000L);

    CurlSlistHandle headerList;
    for (const auto& [key, value] : request.headers) {
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

    if (request.body) {
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
