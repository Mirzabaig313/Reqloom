// Public interface for OS-keychain secret storage. Embedders (desktop secret
// manager, CLI) construct a concrete store via `makeKeychainSecretStore()`
// and use this surface to read / write / remove credentials. The engine
// reads the same store at run time to resolve `{{secret.NAME}}` references.
//
// The concrete QtKeychain-backed implementation stays engine-internal; only
// this abstract surface and the factory are public, so the desktop never
// links QtKeychain directly.
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

    /// Read a named secret. Returns an empty optional if the key is not
    /// present in the keychain; returns `ChainApiError` (code
    /// `SecretAccessFailed`) on backend failure.
    [[nodiscard]] virtual std::expected<std::optional<std::string>, ChainApiError> read(
        const std::string& name) = 0;

    /// Create or overwrite a secret.
    [[nodiscard]] virtual std::expected<void, ChainApiError> write(const std::string& name,
                                                                   const std::string& value) = 0;

    /// Remove a secret. Succeeds (no-op) if the key is already absent.
    [[nodiscard]] virtual std::expected<void, ChainApiError> remove(const std::string& name) = 0;
};

}  // namespace chainapi::engine
