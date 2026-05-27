// HeaderMasking — redact sensitive HTTP headers before they enter the
// observability event stream or persisted run history.
//
// AGENTS.md security rules and Engine Requirement AC-3.6.3 mandate that
// secrets must NEVER appear in logs, telemetry events, or local-storage
// history. The desktop timeline subscribes to `RequestPrepared` events
// and the SQLite history store will persist them; both surfaces consume
// the masked map produced here.
//
// The redaction policy is conservative-by-default:
//   - Header names matching a fixed allowlist of known-sensitive names
//     (Authorization, Cookie, X-API-Key, Proxy-Authorization, …) are
//     fully replaced with a fixed-length placeholder.
//   - Header names CONTAINING (case-insensitive) any of: token, secret,
//     key, password, auth — are also replaced.
//   - Everything else passes through verbatim. Header NAMES are never
//     redacted; only values.
//
// The placeholder (`kRedactedHeaderValue`) is declared in the public
// `chainapi/engine/Events.h` so that timeline renderers, persistence
// layers, and tests share one source of truth. The placeholder is a
// fixed-length string rather than the original value's length, to
// avoid leaking secret length as a side channel.
#pragma once

#include <chainapi/engine/Events.h>

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace chainapi::engine {

/// True when the named header should have its value replaced with
/// `kRedactedHeaderValue`. Match is case-insensitive on the header name.
[[nodiscard]] bool isSensitiveHeader(std::string_view name) noexcept;

/// Return a copy of `headers` with sensitive values replaced.
/// Header NAMES pass through unchanged so the desktop timeline can
/// still show "Authorization" as a row — just not its value.
[[nodiscard]] std::map<std::string, std::string> maskHeaders(
    const std::map<std::string, std::string>& headers);

/// Vector overload — preserves header order and case as the wire saw it.
/// Used for response headers (which the curl client returns as a vector
/// to keep duplicates and order intact).
[[nodiscard]] std::vector<std::pair<std::string, std::string>> maskHeaders(
    const std::vector<std::pair<std::string, std::string>>& headers);

}  // namespace chainapi::engine
