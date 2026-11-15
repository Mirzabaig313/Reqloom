// Cookies — RFC 6265 helpers used by the executor (request-time jar emission,
// response-time jar updates) and the extractor (resolving $.cookies.X from
// Set-Cookie headers).
//
// The per-actor jar in RunContext holds each cookie with the host that set it and
// its Domain, Path, Secure and expiry. A Set-Cookie that expires a cookie
// (Max-Age <= 0, a past Expires, an empty value) removes it (`absorbSetCookies`).
// `shouldSend` / `requestHeaderFor` decide which cookies go on a request. A
// cookie without Path is sent on every path (no RFC default-path).

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
struct JarCookie;
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

/// Apply every `Set-Cookie` in `headers` of the response to `requestUrl`, in order, to
/// `actor`'s jar: store the cookie with its scope, or remove it when the header expires it
/// (`SetCookie::removes`).
void absorbSetCookies(const std::vector<std::pair<std::string, std::string>>& headers,
                      const ActorId& actor,
                      RunContext& ctx,
                      std::string_view requestUrl);

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

/// Whether a jar cookie goes on a request to `requestUrl` at `now`: unexpired, and in scope
/// per `shouldSend` above. A cookie with no setting host is in scope for any host.
[[nodiscard]] bool shouldSend(const JarCookie& cookie,
                              std::string_view requestUrl,
                              std::chrono::system_clock::time_point now);

/// `Cookie:` header value for a request to `requestUrl`: the jar cookies `shouldSend` allows,
/// by name (alphabetical; the newest wins a name clash). Empty when none apply.
[[nodiscard]] std::string requestHeaderFor(
    const std::vector<JarCookie>& jar,
    std::string_view requestUrl,
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now());

/// Emit `name1=v1; name2=v2; ...` for a request `Cookie:` header.
/// Returns empty when the jar is empty. Cookie order follows the
/// jar's internal ordering (alphabetical, since std::map keys are
/// ordered) — RFC 6265 §5.4 says order should not be relied upon, so
/// any deterministic order is acceptable.
[[nodiscard]] std::string formatRequestHeader(const std::map<std::string, std::string>& jar);

}  // namespace reqloom::engine::cookies
