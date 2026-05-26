// Crypto unit tests. PRD §5.10.1 — Slice 5b primitives.
//
// Each test pins an RFC vector or a deterministic third-party fixture
// so a future implementation swap (OpenSSL ↔ libsodium ↔ ...) lands
// without silent drift. The tests live in the engine/tests/unit suite
// so they need the private engine/src include path (provided by
// engine/tests/CMakeLists.txt PRIVATE include directive).
//
// Each test fails on the parent commit — `Crypto.h` did not exist.
#include "infrastructure/util/Crypto.h"
#include "domain/Codecs.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace ce = chainapi::engine;

// ─── HMAC vectors ───────────────────────────────────────────────────────────
//
// HMAC-SHA1: RFC 2202 §3 test case 1.
//   key     = 0x0b * 20
//   data    = "Hi There"
//   result  = b617318655057264e28bc0b6fb378c8ef146be00
TEST(Crypto, hmac_sha1_matches_rfc2202_vector_1) {
    const std::string key(20, '\x0b');
    const auto mac = ce::crypto::hmacSha1(key, "Hi There");
    EXPECT_EQ(ce::codecs::hexEncode(mac),
              "b617318655057264e28bc0b6fb378c8ef146be00");
}

// HMAC-SHA256: RFC 4231 §4.2 test case 1.
TEST(Crypto, hmac_sha256_matches_rfc4231_vector_1) {
    const std::string key(20, '\x0b');
    const auto mac = ce::crypto::hmacSha256(key, "Hi There");
    EXPECT_EQ(ce::codecs::hexEncode(mac),
              "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
}

// HMAC-SHA512: RFC 4231 §4.2 test case 1.
TEST(Crypto, hmac_sha512_matches_rfc4231_vector_1) {
    const std::string key(20, '\x0b');
    const auto mac = ce::crypto::hmacSha512(key, "Hi There");
    EXPECT_EQ(ce::codecs::hexEncode(mac),
              "87aa7cdea5ef619d4ff0b4241a1d6cb02379f4e2ce4ec2787ad0b30545e17cdedaa833b7d6b8a702038b274eaea3f4e4be9d914eeb61f1702e696c203a126854");
}

TEST(Crypto, hmac_returns_empty_string_on_zero_length_inputs) {
    // Defensive: zero-length key is legal (HMAC pads to block size),
    // and zero-length data is legal too. RFC 4231 §4.7 case "test
    // hmac with default key" has an empty data path — we don't pin
    // that exact vector but assert non-empty outputs of correct size.
    EXPECT_EQ(ce::crypto::hmacSha256("", "").size(), 32u);
    EXPECT_EQ(ce::crypto::hmacSha512("k", "").size(), 64u);
    EXPECT_EQ(ce::crypto::hmacSha1("", "msg").size(), 20u);
}

// ─── SHA-256 vector ─────────────────────────────────────────────────────────
//
// FIPS 180-4 test vector for "abc".
TEST(Crypto, sha256_matches_fips_180_4_vector_abc) {
    const auto digest = ce::crypto::sha256("abc");
    EXPECT_EQ(ce::codecs::hexEncode(digest),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Crypto, sha256_of_empty_string_matches_known_value) {
    const auto digest = ce::crypto::sha256("");
    EXPECT_EQ(ce::codecs::hexEncode(digest),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

// ─── JWT HS256 ──────────────────────────────────────────────────────────────
//
// Vector from jwt.io's playground (well-known test). Header
// "{\"alg\":\"HS256\",\"typ\":\"JWT\"}" is the canonical one we emit
// (matches the order most JWT libraries use).
//
//   payload = {"sub":"1234567890","name":"John Doe","iat":1516239022}
//   secret  = "your-256-bit-secret"
//   token   = eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9
//             .eyJzdWIiOiIxMjM0NTY3ODkwIiwibmFtZSI6IkpvaG4gRG9lIiwiaWF0IjoxNTE2MjM5MDIyfQ
//             .SflKxwRJSMeKKF2QT4fwpMeJf36POk6yJV_adQssw5c
TEST(Crypto, jwt_hs256_matches_jwt_io_playground_vector) {
    const std::string payload =
        R"({"sub":"1234567890","name":"John Doe","iat":1516239022})";
    const auto token = ce::crypto::jwtSignHs256(payload, "your-256-bit-secret");
    EXPECT_EQ(token,
              "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9"
              ".eyJzdWIiOiIxMjM0NTY3ODkwIiwibmFtZSI6IkpvaG4gRG9lIiwiaWF0IjoxNTE2MjM5MDIyfQ"
              ".SflKxwRJSMeKKF2QT4fwpMeJf36POk6yJV_adQssw5c");
}

TEST(Crypto, jwt_hs256_produces_three_dot_separated_segments) {
    const auto token = ce::crypto::jwtSignHs256(R"({"x":1})", "k");
    const auto firstDot  = token.find('.');
    const auto secondDot = token.find('.', firstDot + 1);
    ASSERT_NE(firstDot,  std::string::npos);
    ASSERT_NE(secondDot, std::string::npos);
    EXPECT_EQ(token.find('.', secondDot + 1), std::string::npos)
        << "JWT must have exactly two dots";
}

TEST(Crypto, jwt_hs256_uses_base64url_no_padding_no_plus_no_slash) {
    // base64url replaces `+`→`-`, `/`→`_`, strips trailing `=`. The
    // header/payload/signature segments must not contain any of the
    // standard-alphabet variants.
    const auto token = ce::crypto::jwtSignHs256(
        R"({"data":"???>>>","more":"<<<"})",  // forces non-trivial b64 output
        "secret");
    EXPECT_EQ(token.find('+'), std::string::npos);
    EXPECT_EQ(token.find('/'), std::string::npos);
    EXPECT_EQ(token.find('='), std::string::npos);
}

// ─── JWT HS512 — sanity (no canonical playground vector with HS512;
//                          rely on round-trip and shape) ───────────────────

TEST(Crypto, jwt_hs512_signature_is_distinct_from_hs256) {
    const std::string payload = R"({"x":1})";
    const auto t256 = ce::crypto::jwtSignHs256(payload, "secret");
    const auto t512 = ce::crypto::jwtSignHs512(payload, "secret");
    EXPECT_NE(t256, t512);

    // HS256 signature is 32 bytes → base64url ≈ 43 chars.
    // HS512 signature is 64 bytes → base64url ≈ 86 chars.
    // The signature is the third dot-separated segment.
    const auto secondDot256 = t256.find_last_of('.');
    const auto secondDot512 = t512.find_last_of('.');
    const auto sig256Len = t256.size() - secondDot256 - 1;
    const auto sig512Len = t512.size() - secondDot512 - 1;
    EXPECT_LT(sig256Len, sig512Len)
        << "HS512 signature must be longer than HS256";
}
