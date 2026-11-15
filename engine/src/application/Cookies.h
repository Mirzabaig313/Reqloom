// Cookies — RFC 6265 helpers used by the executor (request-time jar emission,
// response-time jar updates) and the extractor (resolving $.cookies.X from
// Set-Cookie headers).
//
// The per-actor jar in RunContext holds name→value pairs. A Set-Cookie that
// expires a cookie (Max-Age <= 0, a past Expires, an empty value) removes it
// (`absorbSetCookies`). `shouldSend` decides host, Secure and Path scope for a
// cookie whose setting host is known.

#pragma once

#include <reqloom/engine/Operation.h>

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace reqloom::engine {
class RunContext;
}  // namespace reqloom::engine

namespace reqloom::engine::cookies {

/// Parse a single `Set-Cookie` header value. Returns `(name, value)`
/// when the header is well-formed (`name=value` followed by zero or
/// more `; attr=...` segments). Returns nullopt on malformed input
/// (no `=`, empty name) — well-known browser-vendor lenience that
/// matches every other cookie parser in the wild.
[[nodiscard]] std::optional<std::pair<std::string, std::string>> parseSetCookie(
    std::string_view header);

/// A `Set-Cookie` header with the attributes the engine honours.
struct SetCookie {
    std::string name;
    std::string value;
    std::optional<std::string> domain;  ///< Lowercase, leading dot removed.
    std::optional<std::string> path;
    bool secure{false};
    std::optional<std::int64_t> maxAge;  ///< Seconds.
    std::optional<std::chrono::sys_seconds> expires;

    /// True when this header deletes the cookie: Max-Age <= 0, an Expires before `now`
    /// (Max-Age wins when both are present, RFC 6265 §5.3 step 3), or an empty value.
    [[nodiscard]] bool removes(std::chrono::system_clock::time_point now) const;
};

/// Parse a `Set-Cookie` header with its attributes (RFC 6265 §5.2). Unknown or malformed
/// attributes are ignored. nullopt under the same conditions as `parseSetCookie`.
[[nodiscard]] std::optional<SetCookie> parseSetCookieHeader(std::string_view header);

/// Walk an HTTP response's headers and return every cookie's
/// `(name, value)` in the order the server emitted them. Later
/// duplicates intentionally appear after earlier ones — the caller
/// decides whether to take last-wins or first-wins semantics.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> collectFromResponse(
    const std::vector<std::pair<std::string, std::string>>& headers);

/// Apply every `Set-Cookie` in `headers`, in order, to `actor`'s jar: store the cookie, or
/// remove it when the header expires it (`SetCookie::removes`).
void absorbSetCookies(const std::vector<std::pair<std::string, std::string>>& headers,
                      const ActorId& actor,
                      RunContext& ctx);

/// Lowercase host of `url` without userinfo or port; empty when the URL has no `://`.
[[nodiscard]] std::string hostOf(std::string_view url);

/// RFC 6265 §5.1.3 domain-match: `host` equals `domain`, or ends with "." + `domain` and is
/// not an IP address. Both are compared case-insensitively.
[[nodiscard]] bool domainMatches(std::string_view host, std::string_view domain);

/// Whether `cookie`, set by a response from `setterHost`, goes on a request to `requestUrl`.
/// Without Domain it is host-only; a Domain the setter may not claim (not a domain-match of
/// the setter) is never sent. Secure cookies need https; Path must path-match (§5.1.4). No
/// public-suffix list is consulted.
[[nodiscard]] bool shouldSend(const SetCookie& cookie,
                              std::string_view setterHost,
                              std::string_view requestUrl);

/// Emit `name1=v1; name2=v2; ...` for a request `Cookie:` header.
/// Returns empty when the jar is empty. Cookie order follows the
/// jar's internal ordering (alphabetical, since std::map keys are
/// ordered) — RFC 6265 §5.4 says order should not be relied upon, so
/// any deterministic order is acceptable.
[[nodiscard]] std::string formatRequestHeader(const std::map<std::string, std::string>& jar);

}  // namespace reqloom::engine::cookies
