// Redacts secret-bearing header and extraction-variable values before they reach events or history.
// Exact allowlist plus substring scan; names pass, values become a fixed-length placeholder so
// length isn't leaked.
#pragma once

#include <reqloom/engine/Events.h>

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace reqloom::engine {

/// True when this header's value should be redacted. Case-insensitive.
[[nodiscard]] bool isSensitiveHeader(std::string_view name) noexcept;

/// True when an identifier (e.g. an `extract:` variable name) looks secret-bearing; shares the
/// substring policy with isSensitiveHeader.
[[nodiscard]] bool isSensitiveName(std::string_view name) noexcept;

/// Agent-facing only (`RunOptions::agentOutput`). True when a header, query parameter or field
/// named `name` carries a credential: exactly `auth` or `key`, or a name containing
/// authorization, cookie, token, secret, password, passwd, apikey, accesskey, privatekey,
/// credential, session, bearer or jwt (case and `-`/`_` ignored).
[[nodiscard]] bool isCredentialName(std::string_view name) noexcept;

/// Agent-facing only. A header whose value is a credential: every header the display policy masks
/// (`isSensitiveHeader`, so `X-Auth-Key`, `Ocp-Apim-Subscription-Key`, `Authentication`) and every
/// `isCredentialName` one.
[[nodiscard]] bool isCredentialHeader(std::string_view name) noexcept;

/// Agent-facing only. A request-body field whose value is a credential: `isCredentialName`, except
/// a field named exactly `key` or `auth` or ending in `type` or `hint` (`{"Key": "Environment"}`,
/// `token_type_hint=access_token`), whose values are labels, not secrets.
[[nodiscard]] bool isCredentialBodyField(std::string_view name) noexcept;

/// Agent-facing only. A response field or extraction whose value is masked by its name: the
/// display policy plus `isCredentialName` (`jwt`, `session_id`).
[[nodiscard]] bool isAgentSensitiveName(std::string_view name) noexcept;

/// Copy of `headers` with sensitive values replaced; names are kept.
[[nodiscard]] std::map<std::string, std::string> maskHeaders(
    const std::map<std::string, std::string>& headers);

/// Vector overload for libcurl's order- and duplicate-preserving response headers.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> maskHeaders(
    const std::vector<std::pair<std::string, std::string>>& headers);

}  // namespace reqloom::engine
