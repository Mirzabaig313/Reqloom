// SecretRedactionTest — pins value-based redaction: the per-run registry of
// secret values (exact, URL-encoded and base64 forms) and the response-body
// field redaction used for failure-detail excerpts.

#include "application/SecretRedaction.h"
#include "domain/Codecs.h"
#include "domain/SecretRegistry.h"

#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <vector>

namespace ce = reqloom::engine;

namespace {

constexpr std::string_view kMarker = ce::kRedactedValueMarker;

[[nodiscard]] bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

// ─── SecretRegistry ─────────────────────────────────────────────────────────

TEST(SecretRegistry, exact_value_is_replaced_by_marker) {
    ce::SecretRegistry registry;
    registry.add("sk_live_abcdef123");
    const auto out = registry.redact("X-Custom: sk_live_abcdef123; next");
    EXPECT_EQ(out, "X-Custom: " + std::string{kMarker} + "; next");
}

TEST(SecretRegistry, every_occurrence_is_replaced) {
    ce::SecretRegistry registry;
    registry.add("abcdef123456");
    const auto out = registry.redact("abcdef123456 and again abcdef123456");
    EXPECT_FALSE(contains(out, "abcdef123456"));
    EXPECT_EQ(out, std::string{kMarker} + " and again " + std::string{kMarker});
}

TEST(SecretRegistry, url_encoded_form_is_replaced) {
    ce::SecretRegistry registry;
    registry.add("p@ss word/+=");
    const auto encoded = ce::codecs::urlEncode("p@ss word/+=");
    ASSERT_NE(encoded, "p@ss word/+=");
    const auto out = registry.redact("https://h/x?token=" + encoded + "&a=1");
    EXPECT_FALSE(contains(out, encoded)) << out;
    EXPECT_TRUE(contains(out, kMarker));
    // Form encoding spells a space as '+'.
    const auto formOut = registry.redact("password=p%40ss+word%2F%2B%3D&x=1");
    EXPECT_FALSE(contains(formOut, "p%40ss+word")) << formOut;
}

TEST(SecretRegistry, base64_form_is_replaced_even_inside_a_basic_header) {
    ce::SecretRegistry registry;
    registry.add("alice:hunter22");
    const auto b64 = ce::codecs::base64Encode("alice:hunter22");
    const auto out = registry.redact("Authorization: Basic " + b64);
    EXPECT_FALSE(contains(out, b64)) << out;
    EXPECT_EQ(out, "Authorization: Basic " + std::string{kMarker});
}

TEST(SecretRegistry, short_and_common_values_are_ignored) {
    ce::SecretRegistry registry;
    registry.add("");
    registry.add("abc");
    registry.add("12345");
    registry.add("true");
    registry.add("false");
    registry.add("null");
    registry.add("      ");
    registry.add("Bearer");
    EXPECT_TRUE(registry.empty());
    const std::string text = "abc 12345 true false null Bearer token";
    EXPECT_EQ(registry.redact(text), text);
}

TEST(SecretRegistry, longest_value_wins_and_overlaps_are_fully_hidden) {
    ce::SecretRegistry registry;
    registry.add("secret-value");
    registry.add("secret-value-extended");
    EXPECT_EQ(registry.redact("x secret-value-extended y"), "x " + std::string{kMarker} + " y");
    EXPECT_EQ(registry.redact("x secret-value y"), "x " + std::string{kMarker} + " y");

    // Two values overlapping each other: neither may leave a visible tail.
    ce::SecretRegistry overlap;
    overlap.add("AAAABBBB");
    overlap.add("BBBBCCCC");
    const auto out = overlap.redact("--AAAABBBBCCCC--");
    EXPECT_EQ(out, "--" + std::string{kMarker} + "--");
}

TEST(SecretRegistry, text_without_secrets_is_returned_unchanged) {
    ce::SecretRegistry registry;
    registry.add("needle-value-1");
    const std::string text = "nothing to see here";
    EXPECT_EQ(registry.redact(text), text);
}

TEST(SecretRegistry, concurrent_add_and_redact_is_safe) {
    ce::SecretRegistry registry;
    std::thread writer([&] {
        for (int i = 0; i < 200; ++i) {
            registry.add("concurrent-secret-" + std::to_string(i));
        }
    });
    std::thread reader([&] {
        for (int i = 0; i < 200; ++i) {
            (void)registry.redact("concurrent-secret-5 and concurrent-secret-150");
        }
    });
    writer.join();
    reader.join();
    EXPECT_FALSE(contains(registry.redact("concurrent-secret-150"), "concurrent-secret-150"));
}

// ─── Response-body field redaction ──────────────────────────────────────────

TEST(BodyFieldRedaction, json_sensitive_keys_are_redacted_at_any_depth) {
    const std::string body =
        R"({"error":"invalid_grant","data":{"access_token":"abc123def456",)"
        R"("nested":[{"refresh_token":"rrr999"},{"client_secret":{"v":"zzz"}}]},"id_token":7})";
    const auto out = ce::redactSensitiveBodyFields(body);
    ASSERT_TRUE(out.has_value());
    EXPECT_TRUE(contains(*out, "invalid_grant")) << *out;
    EXPECT_FALSE(contains(*out, "abc123def456")) << *out;
    EXPECT_FALSE(contains(*out, "rrr999")) << *out;
    EXPECT_FALSE(contains(*out, "zzz")) << *out;
    EXPECT_TRUE(contains(*out, "access_token")) << "key names stay visible";
    EXPECT_TRUE(contains(*out, kMarker));
}

TEST(BodyFieldRedaction, json_key_order_is_preserved) {
    const auto out = ce::redactSensitiveBodyFields(R"({"z":1,"a":2,"password":"pw123456"})");
    ASSERT_TRUE(out.has_value());
    EXPECT_LT(out->find("\"z\""), out->find("\"a\"")) << *out;
    EXPECT_FALSE(contains(*out, "pw123456"));
}

TEST(BodyFieldRedaction, non_json_key_value_pairs_are_redacted) {
    const auto out = ce::redactSensitiveBodyFields(
        "error=invalid_client&access_token=abc123def456&client_secret=s3cr3t-v4lue&x=1");
    ASSERT_TRUE(out.has_value());
    EXPECT_TRUE(contains(*out, "error=invalid_client")) << *out;
    EXPECT_FALSE(contains(*out, "abc123def456")) << *out;
    EXPECT_FALSE(contains(*out, "s3cr3t-v4lue")) << *out;
    EXPECT_TRUE(contains(*out, "x=1")) << *out;
}

TEST(BodyFieldRedaction, non_json_colon_pairs_redact_to_end_of_line) {
    const auto out = ce::redactSensitiveBodyFields(
        "Bad request\nAuthorization: Bearer abc123def456\npassword: \"hunter22\"\nreason: nope");
    ASSERT_TRUE(out.has_value());
    EXPECT_FALSE(contains(*out, "abc123def456")) << *out;
    EXPECT_FALSE(contains(*out, "hunter22")) << *out;
    EXPECT_TRUE(contains(*out, "reason: nope")) << *out;
    EXPECT_TRUE(contains(*out, "Bad request")) << *out;
}

TEST(BodyFieldRedaction, xml_sensitive_elements_are_redacted) {
    const auto out = ce::redactSensitiveBodyFields(
        "<r><error>denied</error><access_token>abc123def456</access_token></r>");
    ASSERT_TRUE(out.has_value());
    EXPECT_FALSE(contains(*out, "abc123def456")) << *out;
    EXPECT_TRUE(contains(*out, "denied")) << *out;
}

TEST(BodyFieldRedaction, binary_body_cannot_be_made_safe) {
    const std::string binary{"\x89PNG\r\n\x1a\n\x00\x00\x00", 11};
    EXPECT_FALSE(ce::redactSensitiveBodyFields(binary).has_value());
}

TEST(BodyExcerpt, excerpt_is_capped_and_masked) {
    ce::SecretRegistry registry;
    registry.add("registered-secret-1");
    const std::string body = R"({"message":"bad registered-secret-1","token":"tok-123456",)" +
                             std::string(400, 'x') + "\"}";
    const auto excerpt = ce::safeBodyExcerpt(body, &registry);
    ASSERT_TRUE(excerpt.has_value());
    EXPECT_FALSE(contains(*excerpt, "registered-secret-1")) << *excerpt;
    EXPECT_FALSE(contains(*excerpt, "tok-123456")) << *excerpt;
    EXPECT_TRUE(contains(*excerpt, "bad")) << *excerpt;
    EXPECT_TRUE(excerpt->ends_with("...")) << *excerpt;
    EXPECT_LE(excerpt->size(), ce::kBodyExcerptBytes + 3);
}

TEST(BodyExcerpt, short_body_is_returned_whole_and_empty_body_gives_nothing) {
    const auto excerpt = ce::safeBodyExcerpt(R"({"error":"nope"})", nullptr);
    ASSERT_TRUE(excerpt.has_value());
    EXPECT_EQ(*excerpt, R"({"error":"nope"})");
    EXPECT_FALSE(ce::safeBodyExcerpt("", nullptr).has_value());
}
