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

TEST(JsonExtractionDetailed, marks_non_jsonpath_sources_as_Unsupported) {
    ce::Extraction headerExt{"Location", "Location", ce::Extraction::Source::Header};

    auto result = ce::extractFromJsonDetailed(ce::OperationId{"x.y"}, R"({"id":"x"})", {headerExt});
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
