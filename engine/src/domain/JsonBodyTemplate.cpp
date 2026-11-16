// JsonBodyTemplate — JSON-aware request body resolution. See header.
#include "JsonBodyTemplate.h"

#include <cctype>
#include <cstddef>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

namespace reqloom::engine {

namespace {

[[nodiscard]] std::string toLowerAscii(std::string_view text) {
    std::string lowered{};
    lowered.reserve(text.size());
    for (const char character : text) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
    return lowered;
}

[[nodiscard]] std::string_view trimWhitespace(std::string_view text) noexcept {
    constexpr std::string_view kWhitespace{" \t\r\n"};
    const auto begin = text.find_first_not_of(kWhitespace);
    if (begin == std::string_view::npos) {
        return {};
    }
    const auto end = text.find_last_not_of(kWhitespace);
    return text.substr(begin, end - begin + 1);
}

[[nodiscard]] bool isJsonMediaType(std::string_view headerValue) {
    const std::string lowered = toLowerAscii(headerValue);
    const std::string_view mediaType =
        trimWhitespace(std::string_view{lowered}.substr(0, lowered.find(';')));
    return mediaType == "application/json" || mediaType.ends_with("+json");
}

void appendPlaceholder(const VariableResolver& resolver,
                       std::string_view placeholder,
                       const bool inString,
                       const RunContext& ctx,
                       const ResolveContext& resolveCtx,
                       VariableResolver::Result& result) {
    auto part = resolver.resolve(placeholder, ctx, resolveCtx);
    // Unresolved placeholders stay verbatim so diagnostics and dry-run markers can find them.
    const bool resolved = part.unresolvedOccurrences.empty();
    result.output += (resolved && inString) ? escapeJsonStringContent(part.output) : part.output;
    result.unresolved.insert(result.unresolved.end(),
                             std::make_move_iterator(part.unresolved.begin()),
                             std::make_move_iterator(part.unresolved.end()));
    result.unresolvedOccurrences.insert(result.unresolvedOccurrences.end(),
                                        std::make_move_iterator(part.unresolvedOccurrences.begin()),
                                        std::make_move_iterator(part.unresolvedOccurrences.end()));
}

}  // namespace

std::optional<std::size_t> findPlaceholderEnd(std::string_view input, std::size_t start) noexcept {
    char quote{};
    bool escaped{};
    for (std::size_t index = start + 2; index < input.size(); ++index) {
        const char character = input[index];
        if (quote != '\0') {
            if (escaped) {
                escaped = false;
            } else if (character == '\\') {
                escaped = true;
            } else if (character == quote) {
                quote = '\0';
            }
            continue;
        }
        if (character == '"' || character == '\'') {
            quote = character;
            continue;
        }
        if (character == '}' && index + 1 < input.size() && input[index + 1] == '}') {
            return index;
        }
    }
    return std::nullopt;
}

std::string escapeJsonStringContent(std::string_view value) {
    std::string escaped{};
    escaped.reserve(value.size());
    for (const char character : value) {
        switch (character) {
            case '"':
                escaped += "\\\"";
                break;
            case '\\':
                escaped += "\\\\";
                break;
            case '\b':
                escaped += "\\b";
                break;
            case '\f':
                escaped += "\\f";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(character) < 0x20U) {
                    escaped += std::format("\\u{:04x}", static_cast<unsigned char>(character));
                } else {
                    escaped.push_back(character);
                }
                break;
        }
    }
    return escaped;
}

bool isJsonBodyTemplate(std::string_view bodyTemplate,
                        const std::map<std::string, std::string>& headers) {
    const std::string_view trimmed = trimWhitespace(bodyTemplate);
    if (trimmed.empty() || (trimmed.front() != '{' && trimmed.front() != '[')) {
        return false;
    }
    for (const auto& [name, value] : headers) {
        if (toLowerAscii(name) == "content-type") {
            return isJsonMediaType(value);
        }
    }
    return true;
}

VariableResolver::Result resolveJsonBodyTemplate(const VariableResolver& resolver,
                                                 std::string_view bodyTemplate,
                                                 const RunContext& ctx,
                                                 const ResolveContext& resolveCtx) {
    VariableResolver::Result result{};
    result.output.reserve(bodyTemplate.size());
    bool inString{};
    bool escaped{};
    std::size_t index{};
    while (index < bodyTemplate.size()) {
        // Placeholders are opaque: their own quotes never flip string state.
        if (bodyTemplate.substr(index, 2) == "{{") {
            const auto end = findPlaceholderEnd(bodyTemplate, index);
            const std::size_t stop = end ? *end + 2 : bodyTemplate.size();
            appendPlaceholder(resolver,
                              bodyTemplate.substr(index, stop - index),
                              inString,
                              ctx,
                              resolveCtx,
                              result);
            escaped = false;
            index = stop;
            continue;
        }
        const char character = bodyTemplate[index];
        result.output.push_back(character);
        if (escaped) {
            escaped = false;
        } else if (inString && character == '\\') {
            escaped = true;
        } else if (character == '"') {
            inString = !inString;
        }
        ++index;
    }
    return result;
}

VariableResolver::Result resolveRequestBody(const VariableResolver& resolver,
                                            std::string_view bodyTemplate,
                                            const std::map<std::string, std::string>& headers,
                                            const RunContext& ctx,
                                            const ResolveContext& resolveCtx) {
    if (isJsonBodyTemplate(bodyTemplate, headers)) {
        return resolveJsonBodyTemplate(resolver, bodyTemplate, ctx, resolveCtx);
    }
    return resolver.resolve(bodyTemplate, ctx, resolveCtx);
}

}  // namespace reqloom::engine
