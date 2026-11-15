// Public interface for OS-keychain secret storage, used by embedders (secret manager, CLI) and read
// by the engine to resolve `{{secret.NAME}}`. The QtKeychain implementation stays internal so the
// desktop never links it.
#pragma once

#include <reqloom/engine/ErrorCodes.h>

#include <expected>
#include <optional>
#include <string>

namespace reqloom::engine {

class SecretStore {
public:
    SecretStore() = default;
    SecretStore(const SecretStore&) = delete;
    SecretStore& operator=(const SecretStore&) = delete;
    SecretStore(SecretStore&&) = delete;
    SecretStore& operator=(SecretStore&&) = delete;
    virtual ~SecretStore() = default;

    /// Read a named secret: an empty optional if absent, `ReqloomError` (`SecretAccessFailed`) on
    /// backend failure.
    [[nodiscard]] virtual std::expected<std::optional<std::string>, ReqloomError> read(
        const std::string& name) = 0;

    /// Create or overwrite a secret.
    [[nodiscard]] virtual std::expected<void, ReqloomError> write(const std::string& name,
                                                                  const std::string& value) = 0;

    /// Remove a secret. Succeeds (no-op) if the key is already absent.
    [[nodiscard]] virtual std::expected<void, ReqloomError> remove(const std::string& name) = 0;

    /// False when no backend exists on this machine, so `read` can only ever report a secret
    /// as absent. The engine then says why a `{{secret.NAME}}` has no value.
    [[nodiscard]] virtual bool available() const noexcept { return true; }
};

}  // namespace reqloom::engine
