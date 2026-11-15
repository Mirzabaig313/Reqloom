// Public predicate API tests — exercise reqloom/engine/Predicate.h, the
// stateless entry point a UI uses for a live "test expression" box. The
// grammar itself is covered by PredicateEvaluatorTest; here we assert the
// public wrapper's contract: bool outcome vs SchemaInvalid on a typo.

#include <reqloom/engine/Predicate.h>

#include <gtest/gtest.h>

namespace ce = reqloom::engine;

namespace {

TEST(EvaluatePredicate, true_when_field_matches) {
    const auto out = ce::evaluatePredicate("$.status == 'COMPLETED'", R"({"status":"COMPLETED"})");
    ASSERT_TRUE(out.has_value()) << out.error().detail;
    EXPECT_TRUE(*out);
}

TEST(EvaluatePredicate, false_when_field_differs) {
    const auto out = ce::evaluatePredicate("$.status == 'COMPLETED'", R"({"status":"PENDING"})");
    ASSERT_TRUE(out.has_value()) << out.error().detail;
    EXPECT_FALSE(*out);
}

TEST(EvaluatePredicate, status_code_is_addressable) {
    const auto out = ce::evaluatePredicate("$.status_code >= 200 && $.status_code < 300", "", 201);
    ASSERT_TRUE(out.has_value()) << out.error().detail;
    EXPECT_TRUE(*out);
}

TEST(EvaluatePredicate, non_json_body_yields_false_not_error) {
    const auto out = ce::evaluatePredicate("$.status == 'ok'", "not json at all");
    ASSERT_TRUE(out.has_value()) << out.error().detail;
    EXPECT_FALSE(*out);
}

TEST(EvaluatePredicate, malformed_expression_reports_schema_invalid) {
    const auto out = ce::evaluatePredicate("$.status ===", R"({"status":"x"})");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, ce::ErrorCode::SchemaInvalid);
}

}  // namespace

TEST(PredicatePublic, validatePredicate_reports_parse_errors_only) {
    EXPECT_TRUE(ce::validatePredicate("($.a == 1 || $.b == 2) && $.c").has_value());
    const auto bad = ce::validatePredicate("$.a ==");
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().code, ce::ErrorCode::SchemaInvalid);
}

TEST(PredicatePublic, validateRegexPattern_applies_the_guard) {
    EXPECT_TRUE(ce::validateRegexPattern(R"(^order \d+ ready$)").has_value());
    EXPECT_FALSE(ce::validateRegexPattern("(a*)*").has_value());
    EXPECT_FALSE(ce::validateRegexPattern("(unclosed").has_value());
}
