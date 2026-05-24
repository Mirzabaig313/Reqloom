// CurlHttpClient — libcurl-backed implementation of HttpClient.
// Phase 1 will implement: connection pooling, per-request timeouts,
// abort-via-multi-handle, TLS verification toggle, proxy config.
#include "CurlHttpClient.h"

namespace chainapi::engine {

CurlHttpClient::CurlHttpClient() = default;
CurlHttpClient::~CurlHttpClient() = default;

std::expected<HttpResponse, ChainApiError>
CurlHttpClient::send(const HttpRequest& /*request*/) {
    return std::unexpected(ChainApiError{
        ErrorCode::NetworkTimeout,
        ErrorClass::Network,
        "CurlHttpClient::send not yet implemented (Phase 1)."
    });
}

}  // namespace chainapi::engine
