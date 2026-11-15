// HttpLlmClient — concrete LlmClient that delegates to an HttpClient.
// The HttpClient reference is non-owning, so production shares one CurlHttpClient with the
// importer.
#pragma once

#include "LlmClient.h"

#include "../http/HttpClient.h"

namespace reqloom::engine {

class HttpLlmClient final : public LlmClient {
public:
    explicit HttpLlmClient(HttpClient& transport) noexcept : transport_(&transport) {}

    std::expected<LlmResponse, ReqloomError> complete(const LlmRequest& request) override;

private:
    HttpClient* transport_{nullptr};
};

}  // namespace reqloom::engine
