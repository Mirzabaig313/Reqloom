// Codecs — pure-stdlib string codecs shared across the engine.
//
// Previously duplicated in AuthStrategy.cpp, ExecutionEngine.cpp, and
// VariableResolver.cpp. Domain-layer placement is correct because:
//   - No I/O, no allocation outside std::string.
//   - No third-party deps beyond stdlib.
//   - Pure functions of their input.
#pragma once

#include <chainapi/engine/Operation.h>

#include <optional>
#include <string>
#include <string_view>

namespace chainapi::engine::codecs {

// ─── base64 (RFC 4648, standard alphabet, with padding) ─────────────────────

/// Encode arbitrary bytes as base64 with padding. Empty input → empty output.
[[nodiscard]] std::string base64Encode(std::string_view input);

/// Decode a base64 string. Returns nullopt for any non-alphabet character
/// (other than padding `=` and ASCII whitespace, which are tolerated),
/// invalid padding placement, or trailing orphan bytes.
[[nodiscard]] std::optional<std::string> base64Decode(std::string_view input);

// ─── hex (lowercase, no separator) ──────────────────────────────────────────

[[nodiscard]] std::string hexEncode(std::string_view input);

/// Decode a hex string. Tolerates mixed case. Returns nullopt for odd
/// length or any non-hex character.
[[nodiscard]] std::optional<std::string> hexDecode(std::string_view input);

// ─── URL component encoding (RFC 3986) ──────────────────────────────────────

/// Percent-encode for URL path / query component scope. Unreserved
/// characters (A-Z a-z 0-9 - _ . ~) pass through; everything else is
/// %HH-encoded with uppercase hex.
[[nodiscard]] std::string urlEncode(std::string_view input);

/// Decode percent-escapes. Accepts both `+` and `%20` for space
/// (application/x-www-form-urlencoded convention). Returns nullopt on
/// any malformed `%`-escape (truncated or non-hex).
[[nodiscard]] std::optional<std::string> urlDecode(std::string_view input);

// ─── HTTP method ─────────────────────────────────────────────────────────────

/// HTTP method as a string literal (e.g. `HttpMethod::Post` → `"POST"`).
/// Shared by the schema writer and the OAuth1 signer.
[[nodiscard]] constexpr std::string_view methodToString(HttpMethod m) noexcept {
    switch (m) {
        case HttpMethod::Get:
            return "GET";
        case HttpMethod::Post:
            return "POST";
        case HttpMethod::Put:
            return "PUT";
        case HttpMethod::Patch:
            return "PATCH";
        case HttpMethod::Delete:
            return "DELETE";
        case HttpMethod::Head:
            return "HEAD";
        case HttpMethod::Options:
            return "OPTIONS";
    }
    return "GET";
}

}  // namespace chainapi::engine::codecs
