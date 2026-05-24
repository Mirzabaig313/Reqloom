#pragma once

#include "SecretStore.h"

namespace chainapi::engine {

class KeychainSecretStore final : public SecretStore {
public:
    KeychainSecretStore();
    ~KeychainSecretStore() override;

    std::expected<std::optional<std::string>, ChainApiError> read(const std::string& name) override;
    std::expected<void, ChainApiError> write(const std::string& name,
                                             const std::string& value) override;
    std::expected<void, ChainApiError> remove(const std::string& name) override;
};

}  // namespace chainapi::engine
