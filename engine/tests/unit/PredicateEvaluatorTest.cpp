// Unit tests for PredicateEvaluator — polling and verifier predicates.
//
// Each test fails on the parent commit (no PredicateEvaluator existed):
// the include itself wouldn't resolve.
#include "application/PredicateEvaluator.h"

#include <reqloom/engine/ErrorCodes.h>

#include <gtest/gtest.h>

#include <string>

namespace ce = reqloom::engine;

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

// ─── Pre-parsed body (parse-once) overload ──────────────────────────────────
//
// The poll loop parses a response body once and evaluates both the success
// and fail predicates against it. The ParsedBody overload must produce the
// same result as the string_view overload for every body shape, and a single
// ParsedBody must be reusable across predicates.

TEST(PredicateEvaluator, parsed_body_overload_matches_string_view_for_json) {
    ce::PredicateEvaluator e;
    const auto pred = e.parse("$.status == 'COMPLETED'");
    ASSERT_TRUE(pred.has_value());

    const std::string body = R"({"status":"COMPLETED"})";
    const auto parsedBody = e.parseBody(body);

    EXPECT_EQ(e.evaluate(*pred, parsedBody), e.evaluate(*pred, body));
    EXPECT_EQ(e.evaluate(*pred, parsedBody), ce::PredicateValue::True);
}

TEST(PredicateEvaluator, parsed_body_overload_matches_string_view_for_status_only) {
    // A non-JSON body must still let $.status_code predicates evaluate —
    // parseBody captures the empty-object fallback, same as the string_view
    // path's internal fallback.
    ce::PredicateEvaluator e;
    const auto pred = e.parse("$.status_code == 200");
    ASSERT_TRUE(pred.has_value());

    const std::string body = "this is not json";
    const auto parsedBody = e.parseBody(body);

    EXPECT_EQ(e.evaluate(*pred, parsedBody, 200), e.evaluate(*pred, body, 200));
    EXPECT_EQ(e.evaluate(*pred, parsedBody, 200), ce::PredicateValue::True);
    EXPECT_EQ(e.evaluate(*pred, parsedBody, 500), ce::PredicateValue::False);
}

TEST(PredicateEvaluator, one_parsed_body_evaluates_against_multiple_predicates) {
    // The reuse contract the poll loop depends on: parse the body once,
    // evaluate a success predicate and a fail predicate against the same
    // ParsedBody, both giving correct independent answers.
    ce::PredicateEvaluator e;
    const auto successPred = e.parse("$.status == 'COMPLETED'");
    const auto failPred = e.parse("$.status in ['FAILED','CANCELLED']");
    ASSERT_TRUE(successPred.has_value());
    ASSERT_TRUE(failPred.has_value());

    const auto inProgress = e.parseBody(R"({"status":"PROCESSING"})");
    EXPECT_EQ(e.evaluate(*successPred, inProgress), ce::PredicateValue::False);
    EXPECT_EQ(e.evaluate(*failPred, inProgress), ce::PredicateValue::False);

    const auto done = e.parseBody(R"({"status":"COMPLETED"})");
    EXPECT_EQ(e.evaluate(*successPred, done), ce::PredicateValue::True);
    EXPECT_EQ(e.evaluate(*failPred, done), ce::PredicateValue::False);

    const auto failed = e.parseBody(R"({"status":"CANCELLED"})");
    EXPECT_EQ(e.evaluate(*successPred, failed), ce::PredicateValue::False);
    EXPECT_EQ(e.evaluate(*failPred, failed), ce::PredicateValue::True);
}

TEST(PredicateEvaluator, parsed_body_overload_is_total_against_unparseable_body) {
    // The noexcept/total contract must hold on the ParsedBody path too.
    ce::PredicateEvaluator e;
    const auto pred = e.parse("$.status == 'COMPLETED'");
    ASSERT_TRUE(pred.has_value());

    std::string giant{"{"};
    giant.append(10'000, 'x');
    const auto parsedBody = e.parseBody(giant);
    EXPECT_EQ(e.evaluate(*pred, parsedBody), ce::PredicateValue::False);
}

// ─── Grouping parentheses (item 4) ──────────────────────────────────────────

TEST(PredicateGrouping, parentheses_override_and_over_or_precedence) {
    // Without parens && binds tighter: a==1 || (b==2 && c).
    const auto body = R"({"a":1,"b":0,"c":false})";
    EXPECT_TRUE(eval("$.a == 1 || $.b == 2 && $.c", body));
    // With parens the && applies to the whole disjunction: c is false.
    EXPECT_FALSE(eval("($.a == 1 || $.b == 2) && $.c", body));
    EXPECT_TRUE(eval("($.a == 1 || $.b == 2) && $.c", R"({"a":0,"b":2,"c":true})"));
}

TEST(PredicateGrouping, nested_groups_and_keyword_operators) {
    const auto body = R"({"a":1,"b":2,"c":3})";
    EXPECT_TRUE(eval("(($.a == 1) and ($.b == 2 or $.c == 0))", body));
    EXPECT_FALSE(eval("($.a == 9 or ($.b == 2 and $.c == 0))", body));
}

TEST(PredicateGrouping, unbalanced_parentheses_are_parse_errors) {
    EXPECT_FALSE(E.parse("($.a == 1").has_value());
    EXPECT_FALSE(E.parse("$.a == 1)").has_value());
    EXPECT_FALSE(E.parse("()").has_value());
    const auto r = E.parse("($.a == 1 || $.b == 2");
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().detail.find("')'"), std::string::npos) << r.error().detail;
}

// ─── Recursion limits (item 5) ──────────────────────────────────────────────

TEST(PredicateLimits, nesting_beyond_limit_is_a_clear_error_not_a_crash) {
    std::string deepParens(100'000, '(');
    deepParens += "$.a";
    deepParens.append(100'000, ')');
    const auto parens = E.parse(deepParens);
    ASSERT_FALSE(parens.has_value());
    EXPECT_NE(parens.error().detail.find("nests deeper than 64"), std::string::npos)
        << parens.error().detail;

    std::string deepArray = "$.a in ";
    deepArray.append(100'000, '[');
    const auto arrays = E.parse(deepArray);
    ASSERT_FALSE(arrays.has_value());
    EXPECT_NE(arrays.error().detail.find("nests deeper than 64"), std::string::npos)
        << arrays.error().detail;
}

TEST(PredicateLimits, nesting_at_the_limit_still_parses) {
    std::string expr(64, '(');
    expr += "$.a == 1";
    expr.append(64, ')');
    EXPECT_TRUE(eval(expr, R"({"a":1})"));
}

TEST(PredicateLimits, very_long_and_chain_parses_evaluates_and_destroys) {
    std::string expr = "$.a == 1";
    for (int i = 0; i < 100'000; ++i) {
        expr += " && $.a == 1";
    }
    EXPECT_TRUE(eval(expr, R"({"a":1})"));
    EXPECT_FALSE(eval(expr + " && $.a == 2", R"({"a":1})"));
}

TEST(PredicateLimits, body_nested_past_limit_is_treated_as_non_json) {
    std::string deep(300, '[');
    deep.append(300, ']');
    // Status-only predicates still work; body paths see an empty object.
    EXPECT_TRUE(eval("$.status_code == 200", deep, 200));
    EXPECT_FALSE(eval("$[0]", deep, 200));
    // 256 levels is accepted.
    std::string ok(256, '[');
    ok.append(256, ']');
    EXPECT_TRUE(eval("$[0]", ok));
}

TEST(PredicateLimits, json_nesting_scan_ignores_brackets_inside_strings) {
    EXPECT_FALSE(ce::jsonNestingExceeds(R"({"a":"[[[[[[[[[["})", 2));
    EXPECT_FALSE(ce::jsonNestingExceeds(R"({"a":"\"[[[[[[["})", 2));
    EXPECT_TRUE(ce::jsonNestingExceeds(R"({"a":[[1]]})", 2));
    EXPECT_FALSE(ce::jsonNestingExceeds(R"({"a":[[1]]})", 3));
}

// ─── Regex guard (item 10) ──────────────────────────────────────────────────

TEST(RegexGuard, nested_unbounded_repeats_are_refused_with_rewrite_advice) {
    for (const char* pattern : {"(a+)+$",
                                "(a*)*",
                                "(a+)*",
                                "(a*)+",
                                "(a{2,})+",
                                "(a+){2,}",
                                "(?:x|y+)+",
                                "((a+))+",
                                "(?<n>a+)+",
                                "(a+?)+"}) {
        const auto r = ce::checkRegexPattern(pattern);
        ASSERT_FALSE(r.has_value()) << pattern;
        EXPECT_NE(r.error().find(pattern), std::string::npos) << r.error();
        EXPECT_NE(r.error().find("make the inner repeat bounded"), std::string::npos) << r.error();
    }
}

TEST(RegexGuard, safe_patterns_are_accepted_and_still_match) {
    for (const char* pattern : {R"(\(a+\)+)",
                                "[(+]+",
                                "(?:ab)+",
                                "(a|b)+",
                                "(a+)b+",
                                R"re("token":"([^"]+)")re",
                                R"(^order \d+ ready$)",
                                "(a+){2}"}) {
        EXPECT_TRUE(ce::checkRegexPattern(pattern).has_value()) << pattern;
    }
    using O = ce::RegexSearchResult::Outcome;
    const auto token = ce::guardedRegexSearch(R"({"token":"abc"})", R"re("token":"([^"]+)")re");
    EXPECT_EQ(token.outcome, O::Matched);
    EXPECT_EQ(token.value, "abc");
    EXPECT_EQ(ce::guardedRegexSearch("order 42 ready", R"(^order \d+ ready$)").outcome, O::Matched);
    EXPECT_EQ(ce::guardedRegexSearch("(aa)", R"(\(a+\)+)").outcome, O::Matched);
    EXPECT_EQ(ce::guardedRegexSearch("x(+y", "[(+]+").value, "(+");
    EXPECT_EQ(ce::guardedRegexSearch("abab", "(?:ab)+").outcome, O::Matched);
    EXPECT_EQ(ce::guardedRegexSearch("zzz", "(a|b)+").outcome, O::NoMatch);
    EXPECT_EQ(ce::guardedRegexSearch("aab", "(a+)b+").value, "aa");
}

TEST(RegexGuard, over_long_pattern_and_oversized_subject_are_refused) {
    const std::string longPattern(ce::kMaxRegexPatternBytes + 1, 'a');
    const auto pr = ce::checkRegexPattern(longPattern);
    ASSERT_FALSE(pr.has_value());
    EXPECT_NE(pr.error().find("1024"), std::string::npos) << pr.error();

    const std::string subject(ce::kMaxRegexSubjectBytes + 1, 'x');
    const auto sr = ce::guardedRegexSearch(subject, "x");
    EXPECT_EQ(sr.outcome, ce::RegexSearchResult::Outcome::Refused);
    EXPECT_NE(sr.reason.find(std::to_string(subject.size())), std::string::npos) << sr.reason;
    EXPECT_NE(sr.reason.find("1048576"), std::string::npos) << sr.reason;
}

TEST(RegexGuard, invalid_pattern_is_refused_not_a_silent_miss) {
    const auto r = ce::guardedRegexSearch("abc", "(unclosed");
    EXPECT_EQ(r.outcome, ce::RegexSearchResult::Outcome::Refused);
    EXPECT_NE(r.reason.find("not a valid pattern"), std::string::npos) << r.reason;
}

TEST(RegexGuard, matches_operator_rejects_guarded_or_invalid_literal_at_parse) {
    const auto nested = E.parse("$.id matches '(a+)+$'");
    ASSERT_FALSE(nested.has_value());
    EXPECT_NE(nested.error().detail.find("(a+)+$"), std::string::npos) << nested.error().detail;
    EXPECT_FALSE(E.parse("$.id matches '(unclosed'").has_value());
    EXPECT_TRUE(eval("$.id matches '^ord_[a-z0-9]+$'", R"({"id":"ord_42"})"));
}
