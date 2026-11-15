// PredicateTemplatesTest — placeholders in predicates are substituted as values.
#include "application/PredicateTemplates.h"

#include <reqloom/engine/Predicate.h>

#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace ce = reqloom::engine;

namespace {

[[nodiscard]] std::string substitute(std::string_view predicate,
                                     const std::map<std::string, std::string>& values) {
    const auto out = ce::substitutePredicateTemplates(
        predicate, [&](std::string_view placeholder) -> std::optional<std::string> {
            if (const auto it = values.find(std::string{placeholder}); it != values.end()) {
                return it->second;
            }
            return std::nullopt;
        });
    return out ? *out : "<unresolved " + out.error() + ">";
}

}  // namespace

TEST(PredicateTemplates, literal_values_outside_quotes_become_bare_operands) {
    EXPECT_EQ(substitute("$.n == {{a.n}}", {{"{{a.n}}", "-12.5"}}), "$.n == -12.5");
    EXPECT_EQ(substitute("$.b == {{a.b}}", {{"{{a.b}}", "true"}}), "$.b == true");
    EXPECT_EQ(substitute("$.z == {{a.z}}", {{"{{a.z}}", "null"}}), "$.z == null");
}

TEST(PredicateTemplates, other_values_outside_quotes_become_one_string_literal) {
    EXPECT_EQ(substitute("$.s == {{a.s}}", {{"{{a.s}}", R"(x" || true || ")"}}),
              R"($.s == "x\" || true || \"")");
    EXPECT_EQ(substitute("$.n == {{a.n}}", {{"{{a.n}}", "1e5"}}), R"($.n == "1e5")");
}

TEST(PredicateTemplates, values_inside_quotes_are_escaped_for_the_literal) {
    EXPECT_EQ(substitute(R"($.s == 'pre-{{a.s}}')", {{"{{a.s}}", R"(it's \ "x")"}}),
              R"($.s == 'pre-it\'s \\ \"x\"')");
}

TEST(PredicateTemplates, placeholder_with_quoted_builtin_argument_is_one_placeholder) {
    EXPECT_EQ(substitute(R"($.s == {{$.fn("a}}b")}})", {{R"({{$.fn("a}}b")}})", "v"}}),
              R"($.s == "v")");
}

TEST(PredicateTemplates, unresolved_placeholder_is_reported) {
    EXPECT_EQ(substitute("$.n == {{a.missing}}", {}), "<unresolved {{a.missing}}>");
}

TEST(PredicateTemplates, load_validation_parses_templated_predicates) {
    EXPECT_TRUE(ce::validateTemplatedPredicate("$.id == {{order.id}}").has_value());
    EXPECT_TRUE(ce::validateTemplatedPredicate(R"($.n == "{{order.name}}")").has_value());
    EXPECT_FALSE(ce::validateTemplatedPredicate("$.id == {{order.id}} &&").has_value());
    EXPECT_FALSE(ce::validateTemplatedPredicate("{{order.id}} {{order.id}}").has_value());
}
