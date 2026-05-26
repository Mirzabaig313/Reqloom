// HttpLlmClient — concrete LlmClient that delegates to an HttpClient.
//
// The HttpClient reference is non-owning so production wiring can share
// the same CurlHttpClient between the run engine and the importer; tests
// substitute a fake.
#pragma once

#include "LlmClient.h"

#include "../http/HttpClient.h"

namespace chainapi::engine {

class HttpLlmClient final : public LlmClient {
public:
    explicit HttpLlmClient(HttpClient& transport) noexcept : transport_(&transport) {}

    std::expected<LlmResponse, ChainApiError> complete(const LlmRequest& request) override;

private:
    HttpClient* transport_{nullptr};
};

}  // namespace chainapi::engine
