// JsonBodyTemplate — JSON-aware resolution of request body templates.
//
// A placeholder inside a JSON string literal has its value escaped as string content, so it
// cannot break out of the string; outside a literal it is inserted raw, so numbers,
// booleans, arrays and objects still work.
#pragma once

#include "VariableResolver.h"

#include <map>
#include <string>
#include <string_view>

namespace reqloom::engine {

/// Escape `value` as JSON string content per RFC 8259 (no surrounding quotes); UTF-8
/// multibyte sequences pass through unchanged.
[[nodiscard]] std::string escapeJsonStringContent(std::string_view value);

/// True when the body is sent as JSON: the template starts with `{` or `[` and any
/// Content-Type is `application/json` or a `+json` type.
[[nodiscard]] bool isJsonBodyTemplate(std::string_view bodyTemplate,
                                      const std::map<std::string, std::string>& headers);

/// Resolve a JSON body template, treating each `{{ ... }}` as an opaque token. Unresolved
/// placeholders stay verbatim, as in `resolve`.
[[nodiscard]] VariableResolver::Result resolveJsonBodyTemplate(const VariableResolver& resolver,
                                                               std::string_view bodyTemplate,
                                                               const RunContext& ctx,
                                                               const ResolveContext& resolveCtx);

/// Resolve a request body: JSON-aware when `isJsonBodyTemplate` holds, else plain `resolve`.
[[nodiscard]] VariableResolver::Result resolveRequestBody(
    const VariableResolver& resolver,
    std::string_view bodyTemplate,
    const std::map<std::string, std::string>& headers,
    const RunContext& ctx,
    const ResolveContext& resolveCtx);

}  // namespace reqloom::engine
