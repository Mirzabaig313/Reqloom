// SecretRedactionTest — pins value-based redaction: the per-run registry of
// secret values (exact, URL-encoded and base64 forms) and the response-body
// field redaction used for failure-detail excerpts.

#include "application/SecretRedaction.h"
#include "domain/Codecs.h"
#include "domain/SecretRegistry.h"

#include <reqloom/engine/ExecutionEngine.h>
#include <reqloom/engine/RunContext.h>

#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <variant>
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

TEST(BodyExcerpt, long_body_is_quoted_whole_up_to_the_default_limit) {
    ce::SecretRegistry registry;
    registry.add("registered-secret-1");
    const std::string body =
        R"({"message":"bad registered-secret-1","token":"tok-123456","pad":")" +
        std::string(3000, 'x') + "\",\"end\":\"LAST-FIELD\"}";
    const auto excerpt = ce::safeBodyExcerpt(body, &registry);
    ASSERT_TRUE(excerpt.has_value());
    EXPECT_FALSE(contains(*excerpt, "registered-secret-1")) << *excerpt;
    EXPECT_FALSE(contains(*excerpt, "tok-123456")) << *excerpt;
    EXPECT_TRUE(contains(*excerpt, "LAST-FIELD")) << "a 3 KB body is not cut";
    EXPECT_FALSE(contains(*excerpt, "truncated")) << *excerpt;
}

TEST(BodyExcerpt, default_limit_is_four_kib) {
    EXPECT_EQ(ce::kDefaultFailureExcerptBytes, 4096U);
    EXPECT_EQ(ce::RunOptions{}.failureExcerptBytes, 4096U);
}

TEST(BodyExcerpt, body_over_the_limit_is_cut_and_states_its_total_size) {
    // 2.3 MiB of text after a masked token.
    const std::string body = "token=tok-123456 " + std::string(2411725, 'y');
    const auto excerpt = ce::safeBodyExcerpt(body, nullptr);
    ASSERT_TRUE(excerpt.has_value());
    EXPECT_FALSE(contains(*excerpt, "tok-123456")) << excerpt->substr(0, 64);
    const std::string marker = "… (truncated, 2.3 MiB total)";
    EXPECT_TRUE(excerpt->ends_with(marker)) << excerpt->substr(excerpt->size() - 64);
    EXPECT_EQ(excerpt->size(), ce::kDefaultFailureExcerptBytes + marker.size());
}

TEST(BodyExcerpt, cut_lands_on_a_utf8_boundary) {
    std::string body{};
    for (int i = 0; i < 3000; ++i) {
        body += "é";
    }
    const auto excerpt = ce::safeBodyExcerpt(body, nullptr, 101);
    ASSERT_TRUE(excerpt.has_value());
    const auto cut = excerpt->substr(0, excerpt->find("… (truncated"));
    EXPECT_EQ(cut.size(), 100U) << "two-byte characters: the 101st byte would split one";
    EXPECT_TRUE(excerpt->ends_with("(truncated, 5.9 KiB total)")) << *excerpt;
}

TEST(BodyExcerpt, limit_is_adjustable_and_zero_gives_nothing) {
    const std::string body = "error=" + std::string(6000, 'z');
    const auto small = ce::safeBodyExcerpt(body, nullptr, 100);
    ASSERT_TRUE(small.has_value());
    EXPECT_TRUE(small->starts_with("error=zzz"));
    EXPECT_TRUE(small->ends_with("… (truncated, 5.9 KiB total)")) << *small;
    EXPECT_EQ(small->find("… (truncated"), 100U);

    const auto large = ce::safeBodyExcerpt(body, nullptr, 8000);
    ASSERT_TRUE(large.has_value());
    EXPECT_EQ(*large, body);

    EXPECT_FALSE(ce::safeBodyExcerpt(body, nullptr, 0).has_value());
}

TEST(BodyExcerpt, limit_above_the_bound_is_clamped) {
    EXPECT_EQ(ce::kMaxFailureExcerptBytes, std::size_t{1024} * 1024);
    const std::string body(std::size_t{1536} * 1024, 'q');
    const auto excerpt = ce::safeBodyExcerpt(body, nullptr, std::size_t{4} * 1024 * 1024);
    ASSERT_TRUE(excerpt.has_value());
    EXPECT_EQ(excerpt->find("… (truncated"), ce::kMaxFailureExcerptBytes);
    EXPECT_TRUE(excerpt->ends_with("(truncated, 1.5 MiB total)"));
}

TEST(BodyExcerpt, masking_applies_at_every_size) {
    ce::SecretRegistry registry;
    registry.add("registered-secret-1");
    const std::string body = R"({"access_token":"tok-123456","note":"registered-secret-1",)"
                             R"("pad":")" +
                             std::string(10000, 'p') + "\"}";
    for (const std::size_t limit :
         {std::size_t{8}, std::size_t{40}, std::size_t{4096}, ce::kMaxFailureExcerptBytes}) {
        const auto excerpt = ce::safeBodyExcerpt(body, &registry, limit);
        ASSERT_TRUE(excerpt.has_value()) << limit;
        EXPECT_FALSE(contains(*excerpt, "tok-123")) << limit << ": " << *excerpt;
        EXPECT_FALSE(contains(*excerpt, "registered-sec")) << limit << ": " << *excerpt;
    }
}

TEST(BodyExcerpt, short_body_is_returned_whole_and_empty_body_gives_nothing) {
    const auto excerpt = ce::safeBodyExcerpt(R"({"error":"nope"})", nullptr);
    ASSERT_TRUE(excerpt.has_value());
    EXPECT_EQ(*excerpt, R"({"error":"nope"})");
    EXPECT_FALSE(ce::safeBodyExcerpt("", nullptr).has_value());
}

// ─── Captured bodies ────────────────────────────────────────────────────────

TEST(CapturedBody, redact_event_leaves_captured_body_as_it_was_and_masks_the_rest) {
    ce::SecretRegistry registry;
    registry.add("registered-secret-1");
    const std::string body = R"({"access_token":"tok-123456","echo":"registered-secret-1"})";
    ce::RunEvent event = ce::ResponseReceived{
        {}, 0, 200, {{"X-Echo", "registered-secret-1"}}, body.size(), {}, {}, body};
    ce::redactEvent(event, registry);
    const auto& response = std::get<ce::ResponseReceived>(event);
    ASSERT_TRUE(response.body.has_value());
    EXPECT_EQ(*response.body, body);
    EXPECT_EQ(response.headers.front().second, kMarker);

    ce::RunEvent failed =
        ce::StepFailed{{}, 0, {}, {}, {}, 1, "HTTP 400 — registered-secret-1", {}, {}};
    ce::redactEvent(failed, registry);
    EXPECT_FALSE(contains(std::get<ce::StepFailed>(failed).detail, "registered-secret-1"));
}

TEST(CapturedBody, agent_facing_mask_hides_token_fields_and_registered_values) {
    ce::RunContext ctx;
    ctx.secretRegistry().add("registered-secret-1");
    const auto masked = ce::maskCapturedBody(
        R"({"access_token":"tok-123456","echo":"registered-secret-1","user":"x"})", ctx);
    ASSERT_TRUE(masked.has_value());
    EXPECT_FALSE(contains(*masked, "tok-123456")) << *masked;
    EXPECT_FALSE(contains(*masked, "registered-secret-1")) << *masked;
    EXPECT_TRUE(contains(*masked, R"("user":"x")")) << *masked;
}

TEST(CapturedBody, agent_facing_mask_refuses_a_body_that_is_not_text) {
    const ce::RunContext ctx;
    const std::string binary{"\x89PNG\r\n\x1a\n\x00\x00\x00", 11};
    EXPECT_FALSE(ce::maskCapturedBody(binary, ctx).has_value());
}

TEST(SecretRedaction, assertion_details_are_value_redacted_in_events_and_step_results) {
    ce::SecretRegistry registry;
    registry.add("sk_live_abcdef123456");
    ce::RunEvent event = ce::AssertionCompleted{.detail = R"($.k was "sk_live_abcdef123456")"};
    ce::redactEvent(event, registry);
    EXPECT_EQ(std::get<ce::AssertionCompleted>(event).detail, R"($.k was "<redacted>")");

    ce::StepResult step;
    step.assertions.push_back({"n", "e", false, R"(expected == "sk_live_abcdef123456")"});
    ce::redactStepResult(step, registry);
    EXPECT_EQ(step.assertions[0].detail, R"(expected == "<redacted>")");
}
