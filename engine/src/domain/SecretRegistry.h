// SecretRegistry — per-run set of secret VALUES that must never leave the
// engine in clear text. Every string bound for an event, StepResult detail, error detail or history
// row goes through `redact()`. Values are registered in several spellings (raw, URL-encoded,
// base64/base64url); short or common values are ignored.
//
// Thread-safe: `add()` takes an exclusive lock, `redact()` a shared one.
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

/// Replaces a registered secret value; same marker as name-based masking.
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

    /// Copy of `text` with every registered spelling replaced by `kRedactedValueMarker`;
    /// overlapping matches merge into one marker.
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
