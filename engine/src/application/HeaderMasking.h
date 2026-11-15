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

/// Copy of `headers` with sensitive values replaced; names are kept.
[[nodiscard]] std::map<std::string, std::string> maskHeaders(
    const std::map<std::string, std::string>& headers);

/// Vector overload for libcurl's order- and duplicate-preserving response headers.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> maskHeaders(
    const std::vector<std::pair<std::string, std::string>>& headers);

}  // namespace reqloom::engine
