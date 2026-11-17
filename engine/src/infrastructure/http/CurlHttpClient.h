#pragma once

#include "HttpClient.h"

#include <string>

namespace reqloom::engine {

class CurlHttpClient final : public HttpClient {
public:
    CurlHttpClient();
    CurlHttpClient(const CurlHttpClient&) = delete;
    CurlHttpClient& operator=(const CurlHttpClient&) = delete;
    CurlHttpClient(CurlHttpClient&&) = delete;
    CurlHttpClient& operator=(CurlHttpClient&&) = delete;
    ~CurlHttpClient() override;

    /// Follows redirects hop by hop (at most 10), so `HttpRequest::onRedirect` sees each hop before
    /// it is sent. Per hop it keeps libcurl's own rules: a 301, 302 or 303 drops the body and keeps
    /// the method; `Authorization` and `Cookie` are dropped once the host, port or scheme differs
    /// from the first request's.
    std::expected<HttpResponse, ReqloomError> send(const HttpRequest& request) override;

private:
    /// One transfer, never following a redirect. `redirectUrl` receives the absolute URL a 3xx
    /// response points to (empty when there is none); `afterHop` says an earlier hop already
    /// reached a server.
    std::expected<HttpResponse, ReqloomError> sendOnce(const HttpRequest& request,
                                                       std::string& redirectUrl,
                                                       bool afterHop);
};

}  // namespace reqloom::engine
