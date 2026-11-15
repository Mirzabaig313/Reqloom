// JsonBodyTemplate — JSON-aware resolution of request body templates.
//
// A placeholder that sits inside a JSON string literal of the template has
// its resolved value escaped as JSON string content, so a value containing
// `"`, `\` or a control character cannot break out of the string. A
// placeholder outside a string literal is inserted raw, so numbers,
// booleans, arrays and objects still work.
#pragma once

#include "VariableResolver.h"

#include <map>
#include <string>
#include <string_view>

namespace reqloom::engine {

/// Escape `value` as JSON string content per RFC 8259 (no surrounding
/// quotes): `"`, `\` and every control character below U+0020. Other bytes,
/// including UTF-8 multibyte sequences, pass through unchanged.
[[nodiscard]] std::string escapeJsonStringContent(std::string_view value);

/// True when the body is sent as JSON: the template (after leading
/// whitespace) starts with `{` or `[`, and any Content-Type header present
/// (matched case-insensitively) is `application/json` or a `+json` type.
[[nodiscard]] bool isJsonBodyTemplate(std::string_view bodyTemplate,
                                      const std::map<std::string, std::string>& headers);

/// Resolve a JSON body template. String state is tracked on the template,
/// with each `{{ ... }}` placeholder treated as an opaque token. Unresolved
/// placeholders stay verbatim and are reported exactly as `resolve` does.
[[nodiscard]] VariableResolver::Result resolveJsonBodyTemplate(const VariableResolver& resolver,
                                                               std::string_view bodyTemplate,
                                                               const RunContext& ctx,
                                                               const ResolveContext& resolveCtx);

/// Resolve a request body: JSON-aware when `isJsonBodyTemplate` holds,
/// otherwise plain `VariableResolver::resolve`.
[[nodiscard]] VariableResolver::Result resolveRequestBody(
    const VariableResolver& resolver,
    std::string_view bodyTemplate,
    const std::map<std::string, std::string>& headers,
    const RunContext& ctx,
    const ResolveContext& resolveCtx);

}  // namespace reqloom::engine
