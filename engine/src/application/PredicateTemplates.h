// `{{…}}` placeholders in assertion and poll predicates, substituted as values.
#pragma once

#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace reqloom::engine {

/// Resolves one placeholder (`{{…}}` text included) to its value, or nullopt when unresolved.
using PredicateValueSource = std::function<std::optional<std::string>(std::string_view)>;

/// Rewrite `predicate` so every placeholder becomes a value that cannot change the
/// expression's structure. Inside a quoted string literal the value is escaped for that
/// literal (`\`, both quotes, `\n`, `\r`, `\t`). Outside one it becomes a single operand: an
/// unquoted number (`-?digits[.digits]`), `true`, `false` or `null` when the value is exactly
/// that text, otherwise a double-quoted string literal.
///
/// @return The rewritten predicate, or the first placeholder `valueOf` could not resolve.
[[nodiscard]] std::expected<std::string, std::string> substitutePredicateTemplates(
    std::string_view predicate, const PredicateValueSource& valueOf);

}  // namespace reqloom::engine
