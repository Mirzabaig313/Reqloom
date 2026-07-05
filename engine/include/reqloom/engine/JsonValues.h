// Public JSON value helpers for embedders (the desktop value picker). Kept
// separate from the engine-internal extraction machinery so the UI can list
// candidate values without reaching into application headers.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace reqloom::engine {

/// Evaluate a JSONPath against `body` and return EVERY matched value as a
/// string (raw for JSON strings, compact form otherwise). Supports field
/// access, `[N]` indexing, the `[*]` wildcard (every array element), and
/// `[?(@.field=='x')]` filters. Returns an empty vector when `body` is not
/// valid JSON or nothing matches. Pure — safe to call from the UI.
[[nodiscard]] std::vector<std::string> extractValues(const std::string& body,
                                                     std::string_view path);

}  // namespace reqloom::engine
