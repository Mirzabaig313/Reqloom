// HeaderMaskingTest — pins the redaction policy for sensitive HTTP
// headers. Engine Requirement AC-3.6.3 mandates secret values must
// never enter the event stream or persisted history; this test is the
// regression surface that proves the masker honours the contract.

#include "application/HeaderMasking.h"

#include <gtest/gtest.h>

#include <map>
#include <string>

namespace ce = chainapi::engine;

// ─── isSensitiveHeader: name classification ─────────────────────────────────

TEST(HeaderMasking, exact_authorization_is_sensitive) {
    EXPECT_TRUE(ce::isSensitiveHeader("Authorization"));
    EXPECT_TRUE(ce::isSensitiveHeader("authorization"));
    EXPECT_TRUE(ce::isSensitiveHeader("AUTHORIZATION"));
}

TEST(HeaderMasking, cookie_and_set_cookie_are_sensitive) {
    EXPECT_TRUE(ce::isSensitiveHeader("Cookie"));
    EXPECT_TRUE(ce::isSensitiveHeader("Set-Cookie"));
}

TEST(HeaderMasking, x_api_key_exact_match_is_sensitive) {
    EXPECT_TRUE(ce::isSensitiveHeader("X-API-Key"));
    EXPECT_TRUE(ce::isSensitiveHeader("x-api-key"));
}

TEST(HeaderMasking, vendor_token_headers_are_sensitive) {
    EXPECT_TRUE(ce::isSensitiveHeader("X-Auth-Token"));
    EXPECT_TRUE(ce::isSensitiveHeader("X-Refresh-Token"));
    EXPECT_TRUE(ce::isSensitiveHeader("X-Stripe-Secret-Key"));
    EXPECT_TRUE(ce::isSensitiveHeader("X-Custom-Password"));
}

TEST(HeaderMasking, content_type_and_friends_are_not_sensitive) {
    EXPECT_FALSE(ce::isSensitiveHeader("Content-Type"));
    EXPECT_FALSE(ce::isSensitiveHeader("Accept"));
    EXPECT_FALSE(ce::isSensitiveHeader("User-Agent"));
    EXPECT_FALSE(ce::isSensitiveHeader("X-Request-Id"));
}

TEST(HeaderMasking, signing_and_encryption_key_headers_are_sensitive) {
    // Regression for the doc/code mismatch: the policy needle is "key",
    // not "apikey", so these vendor key headers must be caught.
    EXPECT_TRUE(ce::isSensitiveHeader("X-Signing-Key"));
    EXPECT_TRUE(ce::isSensitiveHeader("X-Encryption-Key"));
    EXPECT_TRUE(ce::isSensitiveHeader("X-Api-Key"));
}

// ─── isSensitiveName: extraction-variable value redaction ───────────────────

TEST(HeaderMasking, sensitive_variable_names_are_flagged) {
    EXPECT_TRUE(ce::isSensitiveName("token"));
    EXPECT_TRUE(ce::isSensitiveName("access_token"));
    EXPECT_TRUE(ce::isSensitiveName("refreshToken"));
    EXPECT_TRUE(ce::isSensitiveName("api_key"));
    EXPECT_TRUE(ce::isSensitiveName("client_secret"));
    EXPECT_TRUE(ce::isSensitiveName("password"));
}

TEST(HeaderMasking, ordinary_variable_names_are_not_flagged) {
    EXPECT_FALSE(ce::isSensitiveName("order_id"));
    EXPECT_FALSE(ce::isSensitiveName("user_id"));
    EXPECT_FALSE(ce::isSensitiveName("status"));
    EXPECT_FALSE(ce::isSensitiveName("ping_id"));
}

// ─── maskHeaders (map): value redaction ─────────────────────────────────────

TEST(HeaderMasking, sensitive_value_is_replaced_with_fixed_placeholder) {
    std::map<std::string, std::string> headers{
        {"Authorization", "Bearer sk_live_abcdef0123456789"},
        {"Content-Type", "application/json"},
    };
    const auto masked = ce::maskHeaders(headers);

    EXPECT_EQ(masked.at("Authorization"), ce::kRedactedHeaderValue);
    EXPECT_EQ(masked.at("Content-Type"), "application/json");
}

TEST(HeaderMasking, header_names_are_never_redacted) {
    // The desktop timeline shows the NAMES of sensitive headers so the
    // user can see "yes, you are sending Authorization" — only the
    // value is redacted. Pin that the name passes through verbatim.
    std::map<std::string, std::string> headers{
        {"Authorization", "Bearer x"},
        {"X-Stripe-Secret-Key", "sk_live_x"},
    };
    const auto masked = ce::maskHeaders(headers);

    EXPECT_TRUE(masked.contains("Authorization"));
    EXPECT_TRUE(masked.contains("X-Stripe-Secret-Key"));
}

TEST(HeaderMasking, redaction_does_not_leak_value_length) {
    // Fixed-length placeholder defends against length-as-side-channel:
    // a 16-byte vs 32-byte token must not produce different ciphertext
    // sizes downstream.
    std::map<std::string, std::string> shortToken{{"Authorization", "Bearer abc"}};
    std::map<std::string, std::string> longToken{
        {"Authorization", std::string("Bearer ") + std::string(500, 'x')}};

    EXPECT_EQ(ce::maskHeaders(shortToken).at("Authorization"),
              ce::maskHeaders(longToken).at("Authorization"));
}

TEST(HeaderMasking, vector_overload_preserves_order_and_duplicates) {
    // Response headers come in as vector<pair> from libcurl to keep
    // duplicate Set-Cookie order; the masker must preserve that shape.
    std::vector<std::pair<std::string, std::string>> headers{
        {"Date", "Wed, 27 May 2026 12:00:00 GMT"},
        {"Set-Cookie", "session=abc; Path=/"},
        {"Set-Cookie", "csrf=xyz; Path=/"},
        {"Content-Type", "application/json"},
    };
    const auto masked = ce::maskHeaders(headers);

    ASSERT_EQ(masked.size(), 4u);
    EXPECT_EQ(masked[0].first, "Date");
    EXPECT_EQ(masked[1].first, "Set-Cookie");
    EXPECT_EQ(masked[1].second, ce::kRedactedHeaderValue);
    EXPECT_EQ(masked[2].first, "Set-Cookie");
    EXPECT_EQ(masked[2].second, ce::kRedactedHeaderValue);
    EXPECT_EQ(masked[3].first, "Content-Type");
    EXPECT_EQ(masked[3].second, "application/json");
}

TEST(HeaderMasking, empty_input_returns_empty_output) {
    EXPECT_TRUE(ce::maskHeaders(std::map<std::string, std::string>{}).empty());
    EXPECT_TRUE(ce::maskHeaders(std::vector<std::pair<std::string, std::string>>{}).empty());
}
