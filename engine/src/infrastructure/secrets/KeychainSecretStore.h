#pragma once

#include "SecretStore.h"

namespace chainapi::engine {

class KeychainSecretStore final : public SecretStore {
public:
    KeychainSecretStore();
    KeychainSecretStore(const KeychainSecretStore&) = delete;
    KeychainSecretStore& operator=(const KeychainSecretStore&) = delete;
    KeychainSecretStore(KeychainSecretStore&&) = delete;
    KeychainSecretStore& operator=(KeychainSecretStore&&) = delete;
    ~KeychainSecretStore() override;

    /// True when compiled with a real QtKeychain backend, false when the
    /// no-op placeholder is in effect (engine built without QtKeychain).
    [[nodiscard]] static bool backendAvailable() noexcept;

    std::expected<std::optional<std::string>, ChainApiError> read(const std::string& name) override;
    std::expected<void, ChainApiError> write(const std::string& name,
                                             const std::string& value) override;
    std::expected<void, ChainApiError> remove(const std::string& name) override;
};

}  // namespace chainapi::engine
