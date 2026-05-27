// Slice 6d unit tests — extractFromJsonDetailed never short-circuits and
// classifies each extraction as Resolved / Null / Missing / Unsupported.

#include "application/JsonExtraction.h"

#include <gtest/gtest.h>

namespace ce = chainapi::engine;

namespace {

ce::Extraction jsonpath(std::string name, std::string path) {
    return {std::move(name), std::move(path), ce::Extraction::Source::JsonPath};
}

}  // namespace

TEST(JsonExtractionDetailed, classifies_resolved_null_and_missing_per_extraction) {
    const std::string body = R"({
        "id": "abc-123",
        "balance": null,
        "nested": { "deep": "yes" }
    })";

    std::vector<ce::Extraction> extractions = {
        jsonpath("id", "$.id"),
        jsonpath("balance", "$.balance"),
        jsonpath("missing_field", "$.does_not_exist"),
        jsonpath("nested_deep", "$.nested.deep"),
    };

    auto result = ce::extractFromJsonDetailed(ce::OperationId{"acct.get"}, body, extractions);
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    ASSERT_EQ(result->traces.size(), 4u);
    EXPECT_EQ(result->traces[0].outcome, ce::ExtractionTrace::Outcome::Resolved);
    EXPECT_EQ(result->traces[0].value, "abc-123");
    EXPECT_EQ(result->traces[1].outcome, ce::ExtractionTrace::Outcome::Null);
    EXPECT_TRUE(result->traces[1].value.empty());
    EXPECT_EQ(result->traces[2].outcome, ce::ExtractionTrace::Outcome::Missing);
    EXPECT_EQ(result->traces[3].outcome, ce::ExtractionTrace::Outcome::Resolved);
    EXPECT_EQ(result->traces[3].value, "yes");

    EXPECT_EQ(result->values.size(), 2u);  // id + nested_deep
    EXPECT_EQ(result->values.at("id"), "abc-123");
    EXPECT_EQ(result->values.at("nested_deep"), "yes");
}

TEST(JsonExtractionDetailed, surfaces_invalid_json_as_ResponseParse) {
    auto result =
        ce::extractFromJsonDetailed(ce::OperationId{"x.y"}, "not json", {jsonpath("id", "$.id")});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::ResponseParse);
}

TEST(JsonExtractionDetailed, body_only_call_treats_header_extraction_as_missing) {
    // The body-only entry point doesn't have access to response
    // headers, so a Header source surfaces as Missing rather than
    // Unsupported (the path is supported, the data just isn't
    // available). The full-response variant resolves it correctly —
    // see JsonExtractionResponse.* tests below.
    ce::Extraction headerExt{"Location", "$.headers.Location", ce::Extraction::Source::Header};

    auto result = ce::extractFromJsonDetailed(ce::OperationId{"x.y"}, R"({"id":"x"})", {headerExt});
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->traces.size(), 1u);
    EXPECT_EQ(result->traces[0].outcome, ce::ExtractionTrace::Outcome::Missing);
    EXPECT_TRUE(result->values.empty());
}

TEST(JsonExtractionDetailed, xpath_remains_unsupported) {
    // XPath needs an XML parser dep we haven't taken — confirm it
    // still surfaces as Unsupported so users see "this needs schema
    // work" rather than a silent miss.
    ce::Extraction xpathExt{"name", "/root/name", ce::Extraction::Source::XPath};

    auto result = ce::extractFromJsonDetailed(ce::OperationId{"x.y"}, "<x/>", {xpathExt});
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->traces.size(), 1u);
    EXPECT_EQ(result->traces[0].outcome, ce::ExtractionTrace::Outcome::Unsupported);
    EXPECT_TRUE(result->values.empty());
}

TEST(JsonExtractionDetailed, truncates_oversized_resolved_values) {
    std::string longValue(1024, 'x');
    const std::string body = R"({"big":")" + longValue + R"("})";

    auto result =
        ce::extractFromJsonDetailed(ce::OperationId{"x.y"}, body, {jsonpath("big", "$.big")});
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->traces.size(), 1u);
    EXPECT_EQ(result->traces[0].outcome, ce::ExtractionTrace::Outcome::Resolved);
    EXPECT_LE(result->traces[0].value.size(), 260u)
        << "trace value should be truncated for the timeline";
    // The values map keeps the full value — only the trace is truncated.
    EXPECT_EQ(result->values.at("big"), longValue);
}

TEST(JsonExtractionDetailed, empty_extractions_returns_empty_traces) {
    auto result = ce::extractFromJsonDetailed(ce::OperationId{"x.y"}, "{}", {});
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->traces.empty());
    EXPECT_TRUE(result->values.empty());
}

// ─── extractFromResponseDetailed — Header / StatusCode / Cookie / Regex ────

namespace {

ce::Extraction make(std::string name, std::string path, ce::Extraction::Source src) {
    return {std::move(name), std::move(path), src};
}

}  // namespace

TEST(JsonExtractionResponse, header_extraction_resolves_case_insensitively) {
    // Servers are not consistent about header casing; the lookup must
    // be case-insensitive per RFC 7230 §3.2.
    const std::vector<std::pair<std::string, std::string>> headers = {
        {"X-Request-Id", "req-42"},
        {"content-type", "application/json"},
    };
    const std::vector<ce::Extraction> exts = {
        make("rid", "$.headers.x-request-id", ce::Extraction::Source::Header),
        make("ct", "$.headers.Content-Type", ce::Extraction::Source::Header),
    };

    auto result =
        ce::extractFromResponseDetailed(ce::OperationId{"x.y"}, R"({})", 200, headers, exts);
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    EXPECT_EQ(result->traces[0].outcome, ce::ExtractionTrace::Outcome::Resolved);
    EXPECT_EQ(result->values.at("rid"), "req-42");
    EXPECT_EQ(result->traces[1].outcome, ce::ExtractionTrace::Outcome::Resolved);
    EXPECT_EQ(result->values.at("ct"), "application/json");
}

TEST(JsonExtractionResponse, missing_header_marks_missing_not_failure) {
    const std::vector<std::pair<std::string, std::string>> headers = {
        {"Content-Type", "application/json"},
    };
    const std::vector<ce::Extraction> exts = {
        make("nope", "$.headers.X-Not-Here", ce::Extraction::Source::Header),
    };

    auto result =
        ce::extractFromResponseDetailed(ce::OperationId{"x.y"}, R"({})", 200, headers, exts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->traces[0].outcome, ce::ExtractionTrace::Outcome::Missing);
    EXPECT_TRUE(result->values.empty());
}

TEST(JsonExtractionResponse, status_code_resolves_as_string) {
    const std::vector<ce::Extraction> exts = {
        make("status", "$.status_code", ce::Extraction::Source::StatusCode),
    };

    auto result = ce::extractFromResponseDetailed(ce::OperationId{"x.y"}, R"({})", 201, {}, exts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->traces[0].outcome, ce::ExtractionTrace::Outcome::Resolved);
    EXPECT_EQ(result->values.at("status"), "201");
}

TEST(JsonExtractionResponse, cookie_extraction_finds_named_cookie_in_set_cookie) {
    // RFC 6265 §3 — multiple Set-Cookie headers, one cookie per header.
    const std::vector<std::pair<std::string, std::string>> headers = {
        {"Set-Cookie", "SESSIONID=abc-123; Path=/; HttpOnly"},
        {"Set-Cookie", "csrf=token-xyz; Path=/"},
    };
    const std::vector<ce::Extraction> exts = {
        make("session", "$.cookies.SESSIONID", ce::Extraction::Source::Cookie),
        make("csrf", "$.cookies.csrf", ce::Extraction::Source::Cookie),
    };

    auto result =
        ce::extractFromResponseDetailed(ce::OperationId{"x.y"}, R"({})", 200, headers, exts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->values.at("session"), "abc-123");
    EXPECT_EQ(result->values.at("csrf"), "token-xyz");
}

TEST(JsonExtractionResponse, cookie_extraction_strips_attributes) {
    // `; Path=/; Secure; HttpOnly` should not become part of the value.
    const std::vector<std::pair<std::string, std::string>> headers = {
        {"set-cookie", "auth=v1.token.signed; Domain=.example.com; Secure"},
    };
    const std::vector<ce::Extraction> exts = {
        make("auth", "$.cookies.auth", ce::Extraction::Source::Cookie),
    };

    auto result =
        ce::extractFromResponseDetailed(ce::OperationId{"x.y"}, R"({})", 200, headers, exts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->values.at("auth"), "v1.token.signed");
}

TEST(JsonExtractionResponse, missing_cookie_marks_missing) {
    const std::vector<std::pair<std::string, std::string>> headers = {
        {"Set-Cookie", "other=value; Path=/"},
    };
    const std::vector<ce::Extraction> exts = {
        make("session", "$.cookies.SESSIONID", ce::Extraction::Source::Cookie),
    };

    auto result =
        ce::extractFromResponseDetailed(ce::OperationId{"x.y"}, R"({})", 200, headers, exts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->traces[0].outcome, ce::ExtractionTrace::Outcome::Missing);
}

TEST(JsonExtractionResponse, regex_returns_capture_group_when_present) {
    // `(\d+)` against "id=42" → capture group 1 = "42".
    const std::string body = "<html><body>order id=42 created</body></html>";
    const std::vector<ce::Extraction> exts = {
        make("order_id", R"(id=(\d+))", ce::Extraction::Source::Regex),
    };

    auto result = ce::extractFromResponseDetailed(ce::OperationId{"x.y"}, body, 200, {}, exts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->traces[0].outcome, ce::ExtractionTrace::Outcome::Resolved);
    EXPECT_EQ(result->values.at("order_id"), "42");
}

TEST(JsonExtractionResponse, regex_returns_whole_match_when_no_capture_group) {
    const std::string body = "<title>Welcome</title>";
    const std::vector<ce::Extraction> exts = {
        make("title", "Welcome", ce::Extraction::Source::Regex),
    };

    auto result = ce::extractFromResponseDetailed(ce::OperationId{"x.y"}, body, 200, {}, exts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->values.at("title"), "Welcome");
}

TEST(JsonExtractionResponse, malformed_regex_marks_missing_not_failure) {
    // An unclosed group is a regex_error. The extraction must surface
    // as Missing (with a clear trace) rather than crash the run.
    const std::vector<ce::Extraction> exts = {
        make("bad", "(unclosed", ce::Extraction::Source::Regex),
    };

    auto result = ce::extractFromResponseDetailed(ce::OperationId{"x.y"}, "body", 200, {}, exts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->traces[0].outcome, ce::ExtractionTrace::Outcome::Missing);
}

TEST(JsonExtractionResponse, mixed_extractions_each_get_their_own_trace_row) {
    // The classic location-driven flow: server responds 201 with
    // Location header carrying the new resource id and an audit cookie.
    const std::string body = R"({"data":{"id":"ord-42","ts":1700000000}})";
    const std::vector<std::pair<std::string, std::string>> headers = {
        {"Location", "/api/v1/orders/ord-42"},
        {"Set-Cookie", "audit=trace-001; Path=/"},
    };
    const std::vector<ce::Extraction> exts = {
        make("order_id", "$.data.id", ce::Extraction::Source::JsonPath),
        make("location", "$.headers.Location", ce::Extraction::Source::Header),
        make("audit", "$.cookies.audit", ce::Extraction::Source::Cookie),
        make("status", "$.status_code", ce::Extraction::Source::StatusCode),
    };

    auto result = ce::extractFromResponseDetailed(ce::OperationId{"x.y"}, body, 201, headers, exts);
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    EXPECT_EQ(result->traces.size(), 4u);
    EXPECT_EQ(result->values.at("order_id"), "ord-42");
    EXPECT_EQ(result->values.at("location"), "/api/v1/orders/ord-42");
    EXPECT_EQ(result->values.at("audit"), "trace-001");
    EXPECT_EQ(result->values.at("status"), "201");
}

TEST(JsonExtractionResponse, header_extraction_on_non_json_body_does_not_fail_on_parse) {
    // If the only extractions are non-JsonPath, we never need to parse
    // the body. The body can be HTML, plain text, even garbage — and
    // the extraction still succeeds.
    const std::vector<std::pair<std::string, std::string>> headers = {
        {"Content-Type", "text/html"},
    };
    const std::vector<ce::Extraction> exts = {
        make("ct", "$.headers.Content-Type", ce::Extraction::Source::Header),
    };

    auto result = ce::extractFromResponseDetailed(
        ce::OperationId{"x.y"}, "<html>not json</html>", 200, headers, exts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->values.at("ct"), "text/html");
}
