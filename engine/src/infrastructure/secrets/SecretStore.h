// Engine-internal interface for OS-keychain secret storage.
// Concrete impl: KeychainSecretStore (QtKeychain-backed).
#pragma once

#include <chainapi/engine/ErrorCodes.h>

#include <expected>
#include <optional>
#include <string>

namespace chainapi::engine {

class SecretStore {
public:
    SecretStore() = default;
    SecretStore(const SecretStore&) = delete;
    SecretStore& operator=(const SecretStore&) = delete;
    SecretStore(SecretStore&&) = delete;
    SecretStore& operator=(SecretStore&&) = delete;
    virtual ~SecretStore() = default;

    /// Read a named secret. Returns nullopt if not present in the keychain;
    /// returns `ChainApiError` on backend failure.
    virtual std::expected<std::optional<std::string>, ChainApiError> read(
        const std::string& name) = 0;

    virtual std::expected<void, ChainApiError> write(const std::string& name,
                                                     const std::string& value) = 0;

    /// Remove a secret. No-op if missing.
    virtual std::expected<void, ChainApiError> remove(const std::string& name) = 0;
};

}  // namespace chainapi::engine
