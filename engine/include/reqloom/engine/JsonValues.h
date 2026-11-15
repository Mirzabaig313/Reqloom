// Public JSON value helpers for embedders (the desktop value picker).
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace reqloom::engine {

/// Evaluate a JSONPath against `body` and return EVERY matched value as a string (raw for JSON
/// strings, compact otherwise). Supports field access, `[N]`, `[*]` and `[?(@.field=='x')]`. Empty
/// when `body` is not valid JSON or nothing matches.
[[nodiscard]] std::vector<std::string> extractValues(const std::string& body,
                                                     std::string_view path);

}  // namespace reqloom::engine
