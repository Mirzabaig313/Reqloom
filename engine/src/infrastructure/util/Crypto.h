// Crypto — OpenSSL-backed primitives for OAuth1, AWS SigV4, and any
// future strategy that needs HMAC/SHA/JWT.
//
// Infrastructure-layer placement: OpenSSL is a third-party dep; the domain
// layer is std-only. All operations are pure compute (no I/O).
//
// Inputs / outputs are `std::string` carrying raw bytes (NOT hex-encoded).
// Callers that want hex/base64 wrap the result with
// `chainapi::engine::codecs::{hexEncode,base64Encode}`.
//
// All functions are total — they return an empty string on OpenSSL failure
// rather than throwing. Auth strategies can sanity-check the output length
// against the expected digest size.
#pragma once

#include <string>
#include <string_view>

namespace chainapi::engine::crypto {

// ─── HMAC ───────────────────────────────────────────────────────────────────
//
// Output is the raw MAC: 20 bytes for SHA-1, 32 for SHA-256, 64 for SHA-512.

[[nodiscard]] std::string hmacSha1(std::string_view key, std::string_view data);
[[nodiscard]] std::string hmacSha256(std::string_view key, std::string_view data);
[[nodiscard]] std::string hmacSha512(std::string_view key, std::string_view data);

// ─── Plain hash ─────────────────────────────────────────────────────────────

[[nodiscard]] std::string sha256(std::string_view data);

// ─── JWT (HMAC variants only) ───────────────────────────────────────────────
//
// Compose `base64url(header) "." base64url(payload) "." base64url(mac)`
// per RFC 7519 §3. RS256/ES256/etc. need asymmetric-key plumbing and are
// out of scope.

/// HS256: HMAC-SHA256 over the encoded header + payload, signed with `key`.
[[nodiscard]] std::string jwtSignHs256(std::string_view payloadJson, std::string_view key);

/// HS512: same shape, HMAC-SHA512.
[[nodiscard]] std::string jwtSignHs512(std::string_view payloadJson, std::string_view key);

}  // namespace chainapi::engine::crypto
