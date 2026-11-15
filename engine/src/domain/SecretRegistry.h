// SecretRegistry — per-run set of secret VALUES that must never leave the
// engine in clear text.
//
// Name-based masking (HeaderMasking.h) only catches a secret sitting under a
// secret-looking name. This registry closes the gap: every value the engine
// knows to be secret (loaded `{{secret.X}}` values, allowed `{{$.env.NAME}}`
// values, credentials an authenticator obtains or builds, cookie values,
// sensitive extractions) is added here, and every string bound for an event,
// a StepResult detail, an error detail or a history row is passed through
// `redact()`, which replaces each occurrence with `kRedactedValueMarker`.
//
// Each value is registered in several spellings: raw, URL-encoded (RFC 3986
// and form `+`-for-space), and base64 / base64url (padded and unpadded), so a
// token in a query string or a Basic credential is caught too. Values shorter
// than `kMinSecretLength` or trivially common ("true", "Bearer", …) are
// ignored so ordinary text is not shredded.
//
// Thread-safe: `add()` takes an exclusive lock, `redact()` a shared one. The
// desktop runs the engine on a worker thread and may read results elsewhere.
#pragma once

#include <reqloom/engine/Events.h>

#include <array>
#include <cstddef>
#include <set>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

namespace reqloom::engine {

/// Replaces a registered secret value wherever it appears; the same marker
/// as name-based masking.
inline constexpr std::string_view kRedactedValueMarker = kRedactedHeaderValue;

class SecretRegistry {
public:
    /// Values shorter than this are never registered.
    static constexpr std::size_t kMinSecretLength = 6;

    SecretRegistry() = default;
    ~SecretRegistry() = default;
    SecretRegistry(const SecretRegistry&) = delete;
    SecretRegistry& operator=(const SecretRegistry&) = delete;
    SecretRegistry(SecretRegistry&&) = delete;
    SecretRegistry& operator=(SecretRegistry&&) = delete;

    /// True when `value` is long and specific enough to be registered.
    [[nodiscard]] static bool isRegistrable(std::string_view value) noexcept;

    /// Register `value` (and its encoded spellings). No-op for values that
    /// are not registrable or already known.
    void add(std::string_view value);

    /// Copy of `text` with every occurrence of every registered spelling
    /// replaced by `kRedactedValueMarker`. Overlapping matches merge into one
    /// marker, so no fragment of either value survives.
    [[nodiscard]] std::string redact(std::string_view text) const;

    /// True when nothing has been registered.
    [[nodiscard]] bool empty() const;

private:
    void addPatternLocked(std::string pattern);

    mutable std::shared_mutex mutex_;
    std::set<std::string, std::less<>> patterns_;
    /// Patterns bucketed by first byte, each bucket longest-first.
    std::array<std::vector<std::string>, 256> byFirstByte_{};
    std::size_t shortestPattern_{0};
};

}  // namespace reqloom::engine
