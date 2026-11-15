// Codecs — pure-stdlib string codecs shared across the engine.
#pragma once

#include <reqloom/engine/Operation.h>

#include <optional>
#include <string>
#include <string_view>

namespace reqloom::engine::codecs {

/// Encode arbitrary bytes as base64 with padding. Empty input → empty output.
[[nodiscard]] std::string base64Encode(std::string_view input);

/// Decode a base64 string; nullopt for a non-alphabet character (padding `=` and ASCII whitespace
/// are tolerated), bad padding or trailing orphan bytes.
[[nodiscard]] std::optional<std::string> base64Decode(std::string_view input);

[[nodiscard]] std::string hexEncode(std::string_view input);

/// Decode a hex string. Tolerates mixed case. Returns nullopt for odd
/// length or any non-hex character.
[[nodiscard]] std::optional<std::string> hexDecode(std::string_view input);

/// Percent-encode for a URL path / query component; unreserved characters pass through, the rest
/// become uppercase %HH.
[[nodiscard]] std::string urlEncode(std::string_view input);

/// Decode percent-escapes, accepting `+` or `%20` for space; nullopt on a malformed escape.
[[nodiscard]] std::optional<std::string> urlDecode(std::string_view input);

/// Truncate `input` to at most `maxBytes` bytes without splitting a trailing multibyte UTF-8
/// character; pure byte logic, invalid UTF-8 is left as-is.
[[nodiscard]] std::string truncateUtf8(std::string_view input, std::size_t maxBytes);

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

}  // namespace reqloom::engine::codecs
