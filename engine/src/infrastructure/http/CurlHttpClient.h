#pragma once

#include "HttpClient.h"

namespace reqloom::engine {

class CurlHttpClient final : public HttpClient {
public:
    CurlHttpClient();
    CurlHttpClient(const CurlHttpClient&) = delete;
    CurlHttpClient& operator=(const CurlHttpClient&) = delete;
    CurlHttpClient(CurlHttpClient&&) = delete;
    CurlHttpClient& operator=(CurlHttpClient&&) = delete;
    ~CurlHttpClient() override;

    std::expected<HttpResponse, ReqloomError> send(const HttpRequest& request) override;
};

}  // namespace reqloom::engine
