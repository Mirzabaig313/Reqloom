// Redacts secret-bearing HTTP header values (and extraction-variable
// values) before they reach the event stream or persisted history.
// Secrets must never hit logs, telemetry, or disk — AGENTS.md security
// rules.
//
// Policy: an exact allowlist (Authorization, Cookie, X-API-Key, …) plus
// a substring scan (token, secret, password, key, auth). Names pass
// through; only values are replaced — with a fixed-length placeholder,
// so a secret's length isn't leaked as a side channel.
#pragma once

#include <chainapi/engine/Events.h>

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace chainapi::engine {

/// True when this header's value should be redacted. Case-insensitive.
[[nodiscard]] bool isSensitiveHeader(std::string_view name) noexcept;

/// True when an identifier (e.g. an `extract:` variable name) looks
/// secret-bearing. Shares the substring policy with isSensitiveHeader,
/// so an `access_token` extraction and an `Authorization` header are
/// redacted by the same rule.
[[nodiscard]] bool isSensitiveName(std::string_view name) noexcept;

/// Copy of `headers` with sensitive values replaced. Names are kept so
/// the timeline can still show that an Authorization header was sent.
[[nodiscard]] std::map<std::string, std::string> maskHeaders(
    const std::map<std::string, std::string>& headers);

/// Vector overload for response headers, which arrive order- and
/// duplicate-preserving from libcurl.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> maskHeaders(
    const std::vector<std::pair<std::string, std::string>>& headers);

}  // namespace chainapi::engine
