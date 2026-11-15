// Crypto — OpenSSL-backed primitives for OAuth1, AWS SigV4, and any
// future strategy that needs HMAC/SHA/JWT.
//
// Inputs and outputs are `std::string` of raw bytes (not hex); wrap with codecs for hex/base64. All
// functions are total: on OpenSSL failure they return an empty string.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace reqloom::engine::crypto {

// Output is the raw MAC: 20 bytes for SHA-1, 32 for SHA-256, 64 for SHA-512.

[[nodiscard]] std::string hmacSha1(std::string_view key, std::string_view data);
[[nodiscard]] std::string hmacSha256(std::string_view key, std::string_view data);
[[nodiscard]] std::string hmacSha512(std::string_view key, std::string_view data);

[[nodiscard]] std::string sha256(std::string_view data);

/// `count` bytes from the OpenSSL CSPRNG (`RAND_bytes`); empty on failure.
[[nodiscard]] std::string randomBytes(std::size_t count);

// JWT per RFC 7519 §3: base64url(header) "." base64url(payload) "." base64url(mac); HMAC variants
// only.

/// HS256: HMAC-SHA256 over the encoded header + payload, signed with `key`.
[[nodiscard]] std::string jwtSignHs256(std::string_view payloadJson, std::string_view key);

/// HS512: same shape, HMAC-SHA512.
[[nodiscard]] std::string jwtSignHs512(std::string_view payloadJson, std::string_view key);

}  // namespace reqloom::engine::crypto
