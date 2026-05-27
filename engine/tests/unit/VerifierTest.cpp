// Unit tests for Verifier.
//
// Each test fails on the parent commit (no Verifier existed) — the
// include itself wouldn't resolve.
#include "application/Verifier.h"

#include <chainapi/engine/Operation.h>

#include <gtest/gtest.h>

#include <string>

namespace ce = chainapi::engine;

namespace {

ce::Operation makeOp(std::vector<ce::Extraction> exts) {
    ce::Operation op;
    op.id = ce::OperationId{"sample.op"};
    op.resource = ce::ResourceId{"sample"};
    op.extractions = std::move(exts);
    return op;
}

ce::Extraction jsonPath(std::string name, std::string path) {
    return ce::Extraction{std::move(name), std::move(path), ce::Extraction::Source::JsonPath};
}

ce::Extraction header(std::string name, std::string path) {
    return ce::Extraction{std::move(name), std::move(path), ce::Extraction::Source::Header};
}

ce::Extraction statusCode(std::string name) {
    return ce::Extraction{std::move(name), {}, ce::Extraction::Source::StatusCode};
}

}  // namespace

// ─── JsonPath: happy path ───────────────────────────────────────────────────

TEST(Verifier, jsonpath_resolves_to_scalar_marks_verified) {
    ce::Verifier v;
    auto op = makeOp({jsonPath("product_id", "$.data.id")});

    ce::SampleResponse sample;
    sample.body = R"({"data":{"id":"prod-123"}})";
    sample.kind = ce::Provenance::VerifiedAgainst::OpenApiExample;

    auto report = v.verify(op, sample);
    ASSERT_TRUE(report.has_value()) << report.error().detail;
    ASSERT_EQ(report->extractions.size(), 1u);

    const auto& ex = report->extractions[0];
    EXPECT_EQ(ex.variableName, "product_id");
    EXPECT_EQ(ex.status, ce::VerificationStatus::Verified);
    EXPECT_EQ(ex.detail, "\"prod-123\"");
    EXPECT_TRUE(report->allVerified());
    EXPECT_FALSE(report->hasFailures());
}

TEST(Verifier, jsonpath_missing_marks_no_match) {
    ce::Verifier v;
    auto op = makeOp({jsonPath("product_id", "$.data.id")});

    ce::SampleResponse sample;
    sample.body = R"({"data":{"order":{"id":"prod-123"}}})";  // wrong shape

    auto report = v.verify(op, sample);
    ASSERT_TRUE(report);
    EXPECT_EQ(report->extractions[0].status, ce::VerificationStatus::NoMatch);
    EXPECT_TRUE(report->hasFailures());
    EXPECT_FALSE(report->allVerified());
}

TEST(Verifier, jsonpath_resolves_to_explicit_null_marks_null) {
    ce::Verifier v;
    auto op = makeOp({jsonPath("product_id", "$.data.id")});

    ce::SampleResponse sample;
    sample.body = R"({"data":{"id":null}})";

    auto report = v.verify(op, sample);
    ASSERT_TRUE(report);
    EXPECT_EQ(report->extractions[0].status, ce::VerificationStatus::Null);
}

TEST(Verifier, jsonpath_resolves_to_empty_string_marks_null) {
    // Empty-string extractions surfaced the same way
    // as nulls — they're indistinguishable from a usability standpoint.
    ce::Verifier v;
    auto op = makeOp({jsonPath("product_id", "$.data.id")});

    ce::SampleResponse sample;
    sample.body = R"({"data":{"id":""}})";

    auto report = v.verify(op, sample);
    ASSERT_TRUE(report);
    EXPECT_EQ(report->extractions[0].status, ce::VerificationStatus::Null);
    EXPECT_NE(report->extractions[0].detail.find("empty string"), std::string::npos);
}

TEST(Verifier, jsonpath_resolves_to_empty_array_marks_null) {
    ce::Verifier v;
    auto op = makeOp({jsonPath("items", "$.items")});

    ce::SampleResponse sample;
    sample.body = R"({"items":[]})";

    auto report = v.verify(op, sample);
    ASSERT_TRUE(report);
    EXPECT_EQ(report->extractions[0].status, ce::VerificationStatus::Null);
}

TEST(Verifier, jsonpath_supports_array_indices) {
    ce::Verifier v;
    auto op = makeOp({jsonPath("first_id", "$.items[0].id")});

    ce::SampleResponse sample;
    sample.body = R"({"items":[{"id":"first"},{"id":"second"}]})";

    auto report = v.verify(op, sample);
    ASSERT_TRUE(report);
    EXPECT_EQ(report->extractions[0].status, ce::VerificationStatus::Verified);
    EXPECT_EQ(report->extractions[0].detail, "\"first\"");
}

TEST(Verifier, no_sample_body_marks_no_sample_for_jsonpath) {
    ce::Verifier v;
    auto op = makeOp({jsonPath("x", "$.x")});

    ce::SampleResponse sample;  // body is empty by default

    auto report = v.verify(op, sample);
    ASSERT_TRUE(report);
    EXPECT_EQ(report->extractions[0].status, ce::VerificationStatus::NoSample);
    EXPECT_FALSE(report->hasFailures());  // soft state, not a failure
    EXPECT_FALSE(report->allVerified());  // but not verified either
    EXPECT_TRUE(report->noFailures());
}

TEST(Verifier, malformed_json_body_is_treated_as_empty_object) {
    // Body provided but not JSON — verifier falls back gracefully so a
    // text/plain sample doesn't crash the whole operation. JsonPath
    // extractions surface as NoMatch (empty object has no fields).
    ce::Verifier v;
    auto op = makeOp({jsonPath("x", "$.x")});

    ce::SampleResponse sample;
    sample.body = "this is not json";

    auto report = v.verify(op, sample);
    ASSERT_TRUE(report);
    EXPECT_EQ(report->extractions[0].status, ce::VerificationStatus::NoMatch);
}

// ─── Header extractions ─────────────────────────────────────────────────────

TEST(Verifier, header_present_marks_verified_case_insensitive) {
    ce::Verifier v;
    auto op = makeOp({header("etag", "$.headers.ETag")});

    ce::SampleResponse sample;
    sample.headers = {{"content-type", "application/json"},
                      {"etag", "\"abc123\""}};  // server returned lowercase

    auto report = v.verify(op, sample);
    ASSERT_TRUE(report);
    EXPECT_EQ(report->extractions[0].status, ce::VerificationStatus::Verified);
    EXPECT_EQ(report->extractions[0].detail, "\"abc123\"");
}

TEST(Verifier, header_missing_marks_no_match) {
    ce::Verifier v;
    auto op = makeOp({header("etag", "$.headers.ETag")});

    ce::SampleResponse sample;
    sample.headers = {{"content-type", "application/json"}};

    auto report = v.verify(op, sample);
    ASSERT_TRUE(report);
    EXPECT_EQ(report->extractions[0].status, ce::VerificationStatus::NoMatch);
}

TEST(Verifier, header_empty_value_marks_null) {
    ce::Verifier v;
    auto op = makeOp({header("etag", "$.headers.ETag")});

    ce::SampleResponse sample;
    sample.headers = {{"ETag", ""}};

    auto report = v.verify(op, sample);
    ASSERT_TRUE(report);
    EXPECT_EQ(report->extractions[0].status, ce::VerificationStatus::Null);
}

TEST(Verifier, header_with_non_standard_path_marks_not_evaluated) {
    ce::Verifier v;
    auto op = makeOp({header("etag", "ETag")});  // missing $.headers. prefix

    ce::SampleResponse sample;
    sample.headers = {{"ETag", "abc"}};

    auto report = v.verify(op, sample);
    ASSERT_TRUE(report);
    EXPECT_EQ(report->extractions[0].status, ce::VerificationStatus::NotEvaluated);
}

TEST(Verifier, header_with_no_sample_headers_marks_no_sample) {
    ce::Verifier v;
    auto op = makeOp({header("etag", "$.headers.ETag")});

    ce::SampleResponse sample;
    sample.body = R"({"data":{}})";  // body present but no headers map

    auto report = v.verify(op, sample);
    ASSERT_TRUE(report);
    EXPECT_EQ(report->extractions[0].status, ce::VerificationStatus::NoSample);
}

// ─── StatusCode + non-evaluable kinds ───────────────────────────────────────

TEST(Verifier, status_code_marks_verified_when_status_provided) {
    ce::Verifier v;
    auto op = makeOp({statusCode("status")});

    ce::SampleResponse sample;
    sample.statusCode = 200;

    auto report = v.verify(op, sample);
    ASSERT_TRUE(report);
    EXPECT_EQ(report->extractions[0].status, ce::VerificationStatus::Verified);
    EXPECT_EQ(report->extractions[0].detail, "200");
}

TEST(Verifier, status_code_with_zero_marks_no_sample) {
    ce::Verifier v;
    auto op = makeOp({statusCode("status")});

    ce::SampleResponse sample;  // status 0 — unknown

    auto report = v.verify(op, sample);
    ASSERT_TRUE(report);
    EXPECT_EQ(report->extractions[0].status, ce::VerificationStatus::NoSample);
}

TEST(Verifier, xpath_and_regex_and_cookie_mark_not_evaluated) {
    ce::Verifier v;
    auto op = makeOp({});
    op.extractions.push_back({"x", "//foo", ce::Extraction::Source::XPath});
    op.extractions.push_back({"y", "(\\d+)", ce::Extraction::Source::Regex});
    op.extractions.push_back({"z", "session", ce::Extraction::Source::Cookie});

    ce::SampleResponse sample;
    sample.body = R"({"x":1})";

    auto report = v.verify(op, sample);
    ASSERT_TRUE(report);
    for (const auto& ex : report->extractions) {
        EXPECT_EQ(ex.status, ce::VerificationStatus::NotEvaluated) << "name: " << ex.variableName;
    }
    // NotEvaluated is a soft state: verifier doesn't refuse the write,
    // it just records that the importer must surface a warning.
    EXPECT_TRUE(report->noFailures());
    EXPECT_FALSE(report->allVerified());
}

// ─── Aggregate accessors ────────────────────────────────────────────────────

TEST(Verifier, mixed_outcomes_round_trip_through_aggregate_accessors) {
    ce::Verifier v;
    auto op = makeOp({
        jsonPath("good", "$.x"),
        jsonPath("bad", "$.missing"),
        jsonPath("nul", "$.y"),
    });

    ce::SampleResponse sample;
    sample.body = R"({"x":"hit","y":null})";

    auto report = v.verify(op, sample);
    ASSERT_TRUE(report);
    ASSERT_EQ(report->extractions.size(), 3u);
    EXPECT_EQ(report->extractions[0].status, ce::VerificationStatus::Verified);
    EXPECT_EQ(report->extractions[1].status, ce::VerificationStatus::NoMatch);
    EXPECT_EQ(report->extractions[2].status, ce::VerificationStatus::Null);

    EXPECT_FALSE(report->allVerified());
    EXPECT_TRUE(report->hasFailures());  // bad + nul both count as failures
    EXPECT_FALSE(report->noFailures());
}

TEST(Verifier, verify_without_sample_marks_everything_no_sample) {
    ce::Verifier v;
    auto op = makeOp({
        jsonPath("a", "$.a"),
        header("b", "$.headers.B"),
        statusCode("c"),
    });

    auto report = v.verifyWithoutSample(op);
    EXPECT_EQ(report.extractions.size(), 3u);
    for (const auto& ex : report.extractions) {
        EXPECT_EQ(ex.status, ce::VerificationStatus::NoSample);
    }
    EXPECT_TRUE(report.noFailures());
    EXPECT_FALSE(report.allVerified());
}

// ─── Long values are truncated in detail ─────────────────────────────────────

TEST(Verifier, verified_detail_truncates_long_values) {
    ce::Verifier v;
    auto op = makeOp({jsonPath("blob", "$.blob")});

    std::string giant(500, 'x');
    const std::string body = R"({"blob":")" + giant + R"("})";

    ce::SampleResponse sample;
    sample.body = body;

    auto report = v.verify(op, sample);
    ASSERT_TRUE(report);
    EXPECT_EQ(report->extractions[0].status, ce::VerificationStatus::Verified);
    // Truncated to ~80 chars + "...". The exact threshold is an
    // implementation detail; assert "much shorter than 500" instead.
    EXPECT_LT(report->extractions[0].detail.size(), 200u);
}

// ─── Review-fix regression tests ────────────────────────────────────────────

TEST(Verifier, header_lookup_uses_map_comparator_not_linear_scan) {
    // M7 regression. SampleResponse::headers used to be
    // std::map<...,std::less<>> with a manual linear case-fold scan;
    // it now uses CaseInsensitiveLess, so map.find() does the work.
    // Pin the contract: storing the header under any casing must
    // resolve a request that uses any other casing.
    ce::Verifier v;
    auto op = makeOp({header("etag", "$.headers.x-request-id")});

    ce::SampleResponse sample;
    // Server stored the canonical RFC casing — verifier requested
    // lower-case. Pre-fix this happened to work because of the linear
    // scan; post-fix it must work via the comparator.
    sample.headers = {{"X-Request-ID", "req-7"}};

    auto report = v.verify(op, sample);
    ASSERT_TRUE(report);
    EXPECT_EQ(report->extractions[0].status, ce::VerificationStatus::Verified);
    EXPECT_EQ(report->extractions[0].detail, "req-7");
}
