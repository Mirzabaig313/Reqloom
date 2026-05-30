// Cookies — minimal RFC 6265 helpers used by the executor (request-time
// jar emission, response-time jar updates) and the extractor (resolving
// $.cookies.X from Set-Cookie headers).
//
// We intentionally do NOT track domain / path / expiry. The engine is
// a chain runner, not a full HTTP user-agent: keeping the jar to plain
// name→value pairs avoids classes of bugs (cookies leaking across
// hosts) without losing real testing power. Partner schemas that need
// fuller semantics can set their own `Cookie:` header on an operation
// (user headers always win over the auto-jar).

#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace chainapi::engine::cookies {

/// Parse a single `Set-Cookie` header value. Returns `(name, value)`
/// when the header is well-formed (`name=value` followed by zero or
/// more `; attr=...` segments). Returns nullopt on malformed input
/// (no `=`, empty name) — well-known browser-vendor lenience that
/// matches every other cookie parser in the wild.
[[nodiscard]] std::optional<std::pair<std::string, std::string>> parseSetCookie(
    std::string_view header);

/// Walk an HTTP response's headers and return every cookie's
/// `(name, value)` in the order the server emitted them. Later
/// duplicates intentionally appear after earlier ones — the caller
/// decides whether to take last-wins or first-wins semantics.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> collectFromResponse(
    const std::vector<std::pair<std::string, std::string>>& headers);

/// Emit `name1=v1; name2=v2; ...` for a request `Cookie:` header.
/// Returns empty when the jar is empty. Cookie order follows the
/// jar's internal ordering (alphabetical, since std::map keys are
/// ordered) — RFC 6265 §5.4 says order should not be relied upon, so
/// any deterministic order is acceptable.
[[nodiscard]] std::string formatRequestHeader(const std::map<std::string, std::string>& jar);

}  // namespace chainapi::engine::cookies
