#pragma once

#include "HttpClient.h"

namespace chainapi::engine {

class CurlHttpClient final : public HttpClient {
public:
    CurlHttpClient();
    ~CurlHttpClient() override;

    std::expected<HttpResponse, ChainApiError> send(const HttpRequest& request) override;
};

}  // namespace chainapi::engine
