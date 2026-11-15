// Unit tests for JSON-aware request body resolution (JsonBodyTemplate).
#include "domain/JsonBodyTemplate.h"

#include "domain/VariableResolver.h"

#include <reqloom/engine/RunContext.h>

#include <gtest/gtest.h>

#include <map>
#include <string>

#include <nlohmann/json.hpp>

namespace ce = reqloom::engine;

namespace {

class JsonBodyTemplateTest : public ::testing::Test {
protected:
    void SetUp() override {
        rctx_.envVars = {
            {"QUOTE", R"(say "hi")"},
            {"BACKSLASH", R"(C:\temp)"},
            {"NEWLINE", "line1\nline2"},
            {"TAB", "a\tb"},
            {"CONTROL", std::string{"x"} + '\x01' + "y" + '\x1f'},
            {"INJECT", R"(x","role":"admin)"},
            {"TOKEN", "t\"k"},
            {"USER", "u\\n"},
            {"COUNT", "42"},
            {"ENABLED", "true"},
            {"ITEMS", R"(["a","b"])"},
            {"OBJ", R"({"k":"v"})"},
            {"KEY", R"(we"ird)"},
            {"EMPTY", ""},
            {"PLAIN", "hello"},
        };
    }

    [[nodiscard]] ce::VariableResolver::Result resolveJson(const std::string& tmpl) const {
        return ce::resolveJsonBodyTemplate(resolver_, tmpl, ctx_, rctx_);
    }

    ce::VariableResolver resolver_;
    ce::RunContext ctx_;
    ce::ResolveContext rctx_;
};

}  // namespace

TEST(JsonBodyEscape, escapes_quote_backslash_and_control_characters) {
    EXPECT_EQ(ce::escapeJsonStringContent("a\"b\\c"), "a\\\"b\\\\c");
    EXPECT_EQ(ce::escapeJsonStringContent("\n\t\r\b\f"), "\\n\\t\\r\\b\\f");
    EXPECT_EQ(ce::escapeJsonStringContent(std::string{"\x01\x1f", 2}), "\\u0001\\u001f");
    EXPECT_EQ(ce::escapeJsonStringContent("caf\xc3\xa9 /"), "caf\xc3\xa9 /");
    EXPECT_EQ(ce::escapeJsonStringContent(""), "");
}

TEST_F(JsonBodyTemplateTest, escapes_quote_in_string_placeholder) {
    const auto r = resolveJson(R"({"note": "{{env.QUOTE}}"})");
    EXPECT_TRUE(r.unresolved.empty());
    EXPECT_EQ(r.output, R"({"note": "say \"hi\""})");
    EXPECT_EQ(nlohmann::json::parse(r.output)["note"], R"(say "hi")");
}

TEST_F(JsonBodyTemplateTest, escapes_backslash_newline_tab_and_control_characters) {
    const auto r =
        resolveJson(R"({"b": "{{env.BACKSLASH}}", "n": "{{env.NEWLINE}}", "t": "{{env.TAB}}", )"
                    R"("c": "{{env.CONTROL}}"})");
    EXPECT_EQ(r.output,
              R"({"b": "C:\\temp", "n": "line1\nline2", "t": "a\tb", "c": "x\u0001y\u001f"})");
    const auto parsed = nlohmann::json::parse(r.output);
    EXPECT_EQ(parsed["b"], rctx_.envVars.at("BACKSLASH"));
    EXPECT_EQ(parsed["n"], rctx_.envVars.at("NEWLINE"));
    EXPECT_EQ(parsed["t"], rctx_.envVars.at("TAB"));
    EXPECT_EQ(parsed["c"], rctx_.envVars.at("CONTROL"));
}

TEST_F(JsonBodyTemplateTest, injection_attempt_stays_inside_the_string) {
    const auto r = resolveJson(R"({"note": "{{env.INJECT}}"})");
    const auto parsed = nlohmann::json::parse(r.output);
    ASSERT_EQ(parsed.size(), 1u);
    EXPECT_FALSE(parsed.contains("role"));
    EXPECT_EQ(parsed["note"], R"(x","role":"admin)");
}

TEST_F(JsonBodyTemplateTest, escapes_each_of_several_placeholders_in_one_string) {
    const auto r = resolveJson(R"({"auth": "Bearer {{env.TOKEN}} for {{env.USER}}"})");
    EXPECT_EQ(r.output, R"({"auth": "Bearer t\"k for u\\n"})");
    EXPECT_EQ(nlohmann::json::parse(r.output)["auth"], "Bearer t\"k for u\\n");
}

TEST_F(JsonBodyTemplateTest, inserts_values_outside_strings_raw) {
    const auto r =
        resolveJson(R"({"count": {{env.COUNT}}, "flag": {{env.ENABLED}}, "items": {{env.ITEMS}}, )"
                    R"("obj": {{env.OBJ}}})");
    EXPECT_TRUE(r.unresolved.empty());
    EXPECT_EQ(r.output, R"({"count": 42, "flag": true, "items": ["a","b"], "obj": {"k":"v"}})");
}

TEST_F(JsonBodyTemplateTest, escapes_placeholder_used_as_object_key) {
    const auto r = resolveJson(R"({"{{env.KEY}}": 1})");
    EXPECT_EQ(r.output, R"({"we\"ird": 1})");
    EXPECT_EQ(nlohmann::json::parse(r.output)[R"(we"ird)"], 1);
}

TEST_F(JsonBodyTemplateTest, quoted_builtin_argument_does_not_flip_string_state) {
    const auto r = resolveJson(R"({"a": "{{$.base64.encode("a\"b")}}", "n": {{env.COUNT}}})");
    EXPECT_TRUE(r.unresolved.empty());
    // base64("a\"b") == "YSJi"; the count after the string stays raw.
    EXPECT_EQ(r.output, R"({"a": "YSJi", "n": 42})");
}

TEST_F(JsonBodyTemplateTest, escaped_quote_in_literal_text_keeps_string_state) {
    const auto r = resolveJson(R"({"a": "x\"{{env.QUOTE}}", "n": {{env.COUNT}}})");
    EXPECT_EQ(r.output, R"({"a": "x\"say \"hi\"", "n": 42})");
    EXPECT_EQ(nlohmann::json::parse(r.output)["a"], R"(x"say "hi")");
}

TEST_F(JsonBodyTemplateTest, escaped_backslash_before_closing_quote_ends_string) {
    const auto r = resolveJson(R"({"a": "x\\", "n": {{env.ITEMS}}})");
    EXPECT_EQ(r.output, R"({"a": "x\\", "n": ["a","b"]})");
}

TEST_F(JsonBodyTemplateTest, handles_nested_objects_and_arrays) {
    const auto r = resolveJson(
        R"({"outer": {"list": ["{{env.QUOTE}}", {{env.COUNT}}, {"deep": "{{env.NEWLINE}}"}]}, )"
        R"("tail": {{env.OBJ}}})");
    const auto parsed = nlohmann::json::parse(r.output);
    EXPECT_EQ(parsed["outer"]["list"][0], R"(say "hi")");
    EXPECT_EQ(parsed["outer"]["list"][1], 42);
    EXPECT_EQ(parsed["outer"]["list"][2]["deep"], "line1\nline2");
    EXPECT_EQ(parsed["tail"]["k"], "v");
}

TEST_F(JsonBodyTemplateTest, top_level_array_body_is_supported) {
    const auto r = resolveJson(R"([ "{{env.QUOTE}}", {{env.COUNT}} ])");
    EXPECT_EQ(r.output, R"([ "say \"hi\"", 42 ])");
}

TEST_F(JsonBodyTemplateTest, empty_value_produces_empty_string) {
    const auto r = resolveJson(R"({"e": "{{env.EMPTY}}"})");
    EXPECT_TRUE(r.unresolved.empty());
    EXPECT_EQ(r.output, R"({"e": ""})");
}

TEST_F(JsonBodyTemplateTest, unresolved_placeholder_is_kept_and_reported_like_resolve) {
    const std::string tmpl =
        R"({"a": "{{env.MISSING}}", "b": {{other.thing}}, "c": "{{env.QUOTE}}"})";
    const auto r = resolveJson(tmpl);
    const auto plain = resolver_.resolve(tmpl, ctx_, rctx_);
    EXPECT_EQ(r.unresolved, plain.unresolved);
    ASSERT_EQ(r.unresolvedOccurrences.size(), 2u);
    EXPECT_EQ(r.unresolvedOccurrences[0].token, "env.MISSING");
    EXPECT_EQ(r.unresolvedOccurrences[0].component, ce::VariableResolver::Component::Value);
    EXPECT_EQ(r.unresolvedOccurrences[1].token, "other.thing");
    EXPECT_EQ(r.output, R"({"a": "{{env.MISSING}}", "b": {{other.thing}}, "c": "say \"hi\""})");
}

TEST_F(JsonBodyTemplateTest, unterminated_placeholder_is_reported_like_resolve) {
    const std::string tmpl = R"({"a": "{{env.QUOTE"})";
    const auto r = resolveJson(tmpl);
    const auto plain = resolver_.resolve(tmpl, ctx_, rctx_);
    EXPECT_EQ(r.output, plain.output);
    EXPECT_EQ(r.unresolved, plain.unresolved);
    ASSERT_EQ(r.unresolvedOccurrences.size(), plain.unresolvedOccurrences.size());
}

TEST(JsonBodyDetection, json_looking_template_without_content_type_is_json) {
    EXPECT_TRUE(ce::isJsonBodyTemplate("  \n{\"a\":1}", {}));
    EXPECT_TRUE(ce::isJsonBodyTemplate("[1]", {}));
    EXPECT_FALSE(ce::isJsonBodyTemplate("<xml>{{x.y}}</xml>", {}));
    EXPECT_FALSE(ce::isJsonBodyTemplate("plain {{x.y}}", {}));
    EXPECT_FALSE(ce::isJsonBodyTemplate("", {}));
}

TEST(JsonBodyDetection, explicit_content_type_must_be_json) {
    const std::string body = R"({"a": "{{x.y}}"})";
    EXPECT_TRUE(ce::isJsonBodyTemplate(body, {{"content-type", "application/json"}}));
    EXPECT_TRUE(
        ce::isJsonBodyTemplate(body, {{"Content-Type", "Application/JSON; charset=utf-8"}}));
    EXPECT_TRUE(ce::isJsonBodyTemplate(body, {{"Content-Type", "application/vnd.api+json"}}));
    EXPECT_FALSE(ce::isJsonBodyTemplate(body, {{"Content-Type", "text/plain"}}));
    EXPECT_FALSE(ce::isJsonBodyTemplate(body, {{"CONTENT-TYPE", "application/xml"}}));
    EXPECT_TRUE(ce::isJsonBodyTemplate(body, {{"Accept", "text/plain"}}));
}

TEST_F(JsonBodyTemplateTest, non_json_body_is_resolved_exactly_as_today) {
    const std::string text = R"(note={{env.INJECT}})";
    const auto plain = resolver_.resolve(text, ctx_, rctx_);
    const auto viaBody = ce::resolveRequestBody(resolver_, text, {}, ctx_, rctx_);
    EXPECT_EQ(viaBody.output, plain.output);
    EXPECT_EQ(viaBody.output, R"(note=x","role":"admin)");

    const std::string jsonText = R"({"note": "{{env.INJECT}}"})";
    const auto typedPlain =
        ce::resolveRequestBody(resolver_, jsonText, {{"Content-Type", "text/plain"}}, ctx_, rctx_);
    EXPECT_EQ(typedPlain.output, R"({"note": "x","role":"admin"})");
}

TEST_F(JsonBodyTemplateTest, request_body_uses_json_escaping_for_json_bodies) {
    const auto r =
        ce::resolveRequestBody(resolver_, R"({"note": "{{env.INJECT}}"})", {}, ctx_, rctx_);
    EXPECT_EQ(nlohmann::json::parse(r.output)["note"], R"(x","role":"admin)");
}
