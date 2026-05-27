// Unit tests for PredicateEvaluator — polling and verifier predicates.
//
// Each test fails on the parent commit (no PredicateEvaluator existed):
// the include itself wouldn't resolve.
#include "application/PredicateEvaluator.h"

#include <chainapi/engine/ErrorCodes.h>

#include <gtest/gtest.h>

namespace ce = chainapi::engine;

namespace {

ce::PredicateEvaluator E;

bool eval(std::string_view expr, std::string_view body, int status = 0) {
    auto r = E.eval(expr, body, status);
    if (!r) return false;  // parse failure → treated as "expression unsatisfied"
    return *r == ce::PredicateValue::True;
}

}  // namespace

// ─── Equality ───────────────────────────────────────────────────────────────

TEST(PredicateEvaluator, eq_string_matches_completed_status) {
    EXPECT_TRUE(eval("$.status == 'COMPLETED'", R"({"status":"COMPLETED"})"));
    EXPECT_FALSE(eval("$.status == 'COMPLETED'", R"({"status":"PENDING"})"));
}

TEST(PredicateEvaluator, eq_double_quoted_string_works_too) {
    EXPECT_TRUE(eval(R"($.status == "COMPLETED")", R"({"status":"COMPLETED"})"));
}

TEST(PredicateEvaluator, neq_compares_values) {
    EXPECT_TRUE(eval("$.status != 'PENDING'", R"({"status":"COMPLETED"})"));
    EXPECT_FALSE(eval("$.status != 'PENDING'", R"({"status":"PENDING"})"));
}

TEST(PredicateEvaluator, missing_jsonpath_makes_predicate_false_not_error) {
    // Polling-loop totality: a missing field is "not done yet", not a crash.
    EXPECT_FALSE(eval("$.status == 'COMPLETED'", R"({"other":"field"})"));
    EXPECT_FALSE(eval("$.deeply.nested.missing == 'x'", R"({})"));
}

// ─── Numeric comparison ─────────────────────────────────────────────────────

TEST(PredicateEvaluator, numeric_compares) {
    EXPECT_TRUE(eval("$.attempts > 5", R"({"attempts":7})"));
    EXPECT_TRUE(eval("$.attempts >= 7", R"({"attempts":7})"));
    EXPECT_TRUE(eval("$.attempts < 10", R"({"attempts":7})"));
    EXPECT_TRUE(eval("$.attempts <= 7", R"({"attempts":7})"));
    EXPECT_FALSE(eval("$.attempts > 7", R"({"attempts":7})"));
}

TEST(PredicateEvaluator, type_mismatch_compare_is_false_not_error) {
    // Number > string returns false, not a parse error.
    EXPECT_FALSE(eval("$.attempts > 'oops'", R"({"attempts":7})"));
}

TEST(PredicateEvaluator, status_code_shortcut_resolves_from_int) {
    EXPECT_TRUE(eval("$.status_code == 200", R"({})", 200));
    EXPECT_FALSE(eval("$.status_code == 200", R"({})", 404));
    // status_code shortcut is overridden if the body has the field.
    EXPECT_TRUE(eval("$.status_code == 1", R"({"status_code":1})", 200));
}

// ─── In / membership ────────────────────────────────────────────────────────

TEST(PredicateEvaluator, in_operator_matches_any_array_member) {
    EXPECT_TRUE(eval("$.status in ['FAILED','CANCELLED','EXPIRED']", R"({"status":"CANCELLED"})"));
    EXPECT_FALSE(eval("$.status in ['FAILED','CANCELLED']", R"({"status":"PROCESSING"})"));
}

TEST(PredicateEvaluator, in_operator_works_for_status_code) {
    EXPECT_TRUE(eval("$.status_code in [200, 201, 202]", R"({})", 201));
    EXPECT_FALSE(eval("$.status_code in [200, 201]", R"({})", 500));
}

// ─── Matches / regex ────────────────────────────────────────────────────────

TEST(PredicateEvaluator, matches_uses_regex_search) {
    EXPECT_TRUE(eval("$.message matches 'completed.*successfully'",
                     R"({"message":"job completed successfully"})"));
    EXPECT_FALSE(
        eval("$.message matches '^completed$'", R"({"message":"job completed successfully"})"));
}

TEST(PredicateEvaluator, matches_with_invalid_regex_is_false_not_error) {
    EXPECT_FALSE(eval("$.message matches '['",  // unterminated character class
                      R"({"message":"x"})"));
}

// ─── Logical operators ──────────────────────────────────────────────────────

TEST(PredicateEvaluator, and_short_circuits_left_to_right) {
    EXPECT_TRUE(
        eval("$.status == 'COMPLETED' && $.code == 0", R"({"status":"COMPLETED","code":0})"));
    EXPECT_FALSE(
        eval("$.status == 'COMPLETED' && $.code == 0", R"({"status":"COMPLETED","code":1})"));
    // First side false → second not consulted (no missing-field error).
    EXPECT_FALSE(
        eval("$.status == 'PENDING' && $.never_referenced == 0", R"({"status":"COMPLETED"})"));
}

TEST(PredicateEvaluator, or_short_circuits) {
    EXPECT_TRUE(eval("$.status == 'OK' || $.code == 200", R"({"code":200})"));
}

// ─── Truthiness (bare jsonpath) ─────────────────────────────────────────────

TEST(PredicateEvaluator, bare_jsonpath_is_truthy_when_present_and_non_null) {
    EXPECT_TRUE(eval("$.data.id", R"({"data":{"id":"abc"}})"));
    EXPECT_FALSE(eval("$.data.id", R"({"data":{"id":null}})"));
    EXPECT_FALSE(eval("$.data.id", R"({"data":{}})"));
    // Empty string is falsy.
    EXPECT_FALSE(eval("$.data.id", R"({"data":{"id":""}})"));
    // Number 0 is falsy.
    EXPECT_FALSE(eval("$.attempts", R"({"attempts":0})"));
    // Empty array/object → falsy.
    EXPECT_FALSE(eval("$.items", R"({"items":[]})"));
    EXPECT_FALSE(eval("$.items", R"({"items":{}})"));
}

TEST(PredicateEvaluator, truthiness_supports_verifier_use_case) {
    // Verification: "is the AI's proposed extract path
    // present and non-null in the sample response?" reduces to a
    // bare-jsonpath truthiness check.
    EXPECT_TRUE(eval("$.data.product_id", R"({"data":{"product_id":"prod-123"}})"));
    EXPECT_FALSE(eval("$.data.product_id", R"({"data":{"order":{"id":"x"}}})"));
}

// ─── Indexed paths ──────────────────────────────────────────────────────────

TEST(PredicateEvaluator, jsonpath_supports_array_indices) {
    EXPECT_TRUE(eval("$.items[0].id == 'first'", R"({"items":[{"id":"first"},{"id":"second"}]})"));
    EXPECT_TRUE(eval("$.items[1].id == 'second'", R"({"items":[{"id":"first"},{"id":"second"}]})"));
    EXPECT_FALSE(eval("$.items[5].id == 'oops'", R"({"items":[{"id":"first"}]})"));
}

TEST(PredicateEvaluator, jsonpath_supports_quoted_keys) {
    // Keys that aren't valid identifiers need bracket-with-quote form.
    EXPECT_TRUE(eval(R"($["weird-key"] == 1)", R"({"weird-key":1})"));
}

// ─── Parse errors ───────────────────────────────────────────────────────────

TEST(PredicateEvaluator, malformed_expression_returns_parse_error) {
    auto r = E.parse("$.status ===");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ce::ErrorCode::SchemaInvalid);
    EXPECT_EQ(r.error().cls, ce::ErrorClass::Schema);
}

TEST(PredicateEvaluator, dangling_bracket_is_parse_error) {
    auto r = E.parse("$.items[");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ce::ErrorCode::SchemaInvalid);
}

TEST(PredicateEvaluator, trailing_garbage_is_parse_error) {
    auto r = E.parse("$.x == 1 garbage");
    EXPECT_FALSE(r.has_value());
}

// ─── Reuse / parsed predicate stays valid across calls ──────────────────────

TEST(PredicateEvaluator, parsed_predicate_can_evaluate_many_bodies) {
    auto parsed = E.parse("$.status == 'COMPLETED'");
    ASSERT_TRUE(parsed.has_value());

    EXPECT_EQ(E.evaluate(*parsed, R"({"status":"PENDING"})"), ce::PredicateValue::False);
    EXPECT_EQ(E.evaluate(*parsed, R"({"status":"COMPLETED"})"), ce::PredicateValue::True);
}

TEST(PredicateEvaluator, malformed_body_does_not_crash_evaluation) {
    auto parsed = E.parse("$.status_code == 200");
    ASSERT_TRUE(parsed.has_value());

    // Body is not JSON; predicate over status_code still works.
    EXPECT_EQ(E.evaluate(*parsed, "this is not json", 200), ce::PredicateValue::True);
    EXPECT_EQ(E.evaluate(*parsed, "this is not json", 500), ce::PredicateValue::False);
}

// ─── Review-fix regression tests ────────────────────────────────────────────

TEST(PredicateEvaluator, evaluate_is_total_against_unparseable_body) {
    // H1 regression. The function is declared noexcept; any throw —
    // including the bad_alloc paths inside nlohmann::json — must
    // collapse to False rather than std::terminate.
    ce::PredicateEvaluator e;
    auto parsed = e.parse("$.status == 'COMPLETED'");
    ASSERT_TRUE(parsed.has_value());

    // Garbage that's *almost* JSON: opens an object, never closes it,
    // and contains lots of bytes so the parser walks far enough to
    // touch every error path. evaluate() must not throw.
    std::string giant{"{"};
    giant.append(10'000, 'x');
    EXPECT_EQ(e.evaluate(*parsed, giant), ce::PredicateValue::False);
}

TEST(PredicateEvaluator, integer_compare_preserves_int64_precision) {
    // M1 regression. Real-world IDs (Stripe payment_id, Twitter
    // snowflake) often exceed 2^53 — the mantissa of double. Comparing
    // via double would round these to equal values; integer compare
    // preserves the difference.
    ce::PredicateEvaluator e;

    // 9007199254740993 == 2^53 + 1 — first integer not representable
    // exactly as a double.
    EXPECT_EQ(e.eval("$.id > 9007199254740992", R"({"id":9007199254740993})").value(),
              ce::PredicateValue::True);
    EXPECT_EQ(e.eval("$.id < 9007199254740994", R"({"id":9007199254740993})").value(),
              ce::PredicateValue::True);

    // Same comparison with floats falls back to double — must still work.
    EXPECT_EQ(e.eval("$.x < 1.5", R"({"x":1.0})").value(), ce::PredicateValue::True);
}
