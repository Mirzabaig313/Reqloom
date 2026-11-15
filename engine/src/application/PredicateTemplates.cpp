// `{{…}}` placeholders in assertion and poll predicates, substituted as values.

#include "PredicateTemplates.h"

#include "../domain/JsonBodyTemplate.h"

#include <reqloom/engine/Predicate.h>

#include <cctype>
#include <string>
#include <string_view>

namespace reqloom::engine {

namespace {

/// Escape `value` for a predicate string literal (the lexer's `readString` rules).
[[nodiscard]] std::string escapeForLiteral(const std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\'':
                out += "\\'";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out.push_back(c);
                break;
        }
    }
    return out;
}

[[nodiscard]] bool isPlainNumber(const std::string_view text) noexcept {
    std::size_t i = text.starts_with('-') ? 1U : 0U;
    const auto digits = [&] {
        const auto start = i;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])) != 0) {
            ++i;
        }
        return i > start;
    };
    if (!digits()) {
        return false;
    }
    if (i < text.size() && text[i] == '.') {
        ++i;
        if (!digits()) {
            return false;
        }
    }
    return i == text.size();
}

[[nodiscard]] std::string asOperand(const std::string& value) {
    if (isPlainNumber(value) || value == "true" || value == "false" || value == "null") {
        return value;
    }
    return '"' + escapeForLiteral(value) + '"';
}

}  // namespace

std::expected<std::string, std::string> substitutePredicateTemplates(
    const std::string_view predicate, const PredicateValueSource& valueOf) {
    std::string out;
    out.reserve(predicate.size());
    char quote{};
    for (std::size_t i = 0; i < predicate.size(); ++i) {
        const char c = predicate[i];
        if (c == '{' && i + 1 < predicate.size() && predicate[i + 1] == '{') {
            const auto end = findPlaceholderEnd(predicate, i);
            if (!end) {
                out.append(predicate.substr(i));
                break;
            }
            const auto placeholder = predicate.substr(i, *end + 2 - i);
            const auto value = valueOf(placeholder);
            if (!value) {
                return std::unexpected(std::string{placeholder});
            }
            out += quote != '\0' ? escapeForLiteral(*value) : asOperand(*value);
            i = *end + 1;
            continue;
        }
        out.push_back(c);
        if (quote != '\0') {
            if (c == '\\' && i + 1 < predicate.size()) {
                out.push_back(predicate[++i]);
            } else if (c == quote) {
                quote = '\0';
            }
        } else if (c == '"' || c == '\'') {
            quote = c;
        }
    }
    return out;
}

std::expected<void, ReqloomError> validateTemplatedPredicate(const std::string_view expression) {
    const auto neutral = substitutePredicateTemplates(
        expression, [](std::string_view) { return std::optional<std::string>{"x"}; });
    return validatePredicate(neutral ? *neutral : std::string{expression});
}

}  // namespace reqloom::engine
