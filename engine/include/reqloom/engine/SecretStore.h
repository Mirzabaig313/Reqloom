// Public interface for OS-keychain secret storage, used by embedders (secret manager, CLI) and read
// by the engine to resolve `{{secret.NAME}}`. The QtKeychain implementation stays internal so the
// desktop never links it.
#pragma once

#include <reqloom/engine/ErrorCodes.h>

#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace reqloom::engine {

/// Prefix of the environment variable that supplies `{{secret.NAME}}` without a keychain.
inline constexpr std::string_view kSecretEnvironmentPrefix = "REQLOOM_SECRET_";

/// The environment variable that supplies `{{secret.<name>}}`: `REQLOOM_SECRET_` followed by the
/// name in upper case (`api_key` → `REQLOOM_SECRET_API_KEY`). nullopt when the name is empty or
/// has a character other than an ASCII letter, digit or `_`; such a secret can come only from the
/// keychain. The bare `<name>` variable is never read. Two names that differ only in case share
/// one variable; a run that would read it for both fails instead.
[[nodiscard]] std::optional<std::string> secretEnvironmentVariable(std::string_view name);

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

    /// True when the most recent `read` could not reach a keychain on this machine (no session
    /// bus, no event loop) and so reported the secret absent. Per read: the next read tries again.
    [[nodiscard]] virtual bool lastReadUnreachable() const noexcept { return false; }
};

}  // namespace reqloom::engine
