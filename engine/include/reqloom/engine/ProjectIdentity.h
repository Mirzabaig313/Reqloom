// Project identity: the stable `id:` that namespaces a project's keychain
// secrets so one project cannot read another's.
#pragma once

#include <reqloom/engine/ErrorCodes.h>

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

namespace reqloom::engine {

struct Project;

/// Upper bound on a project id's length, in bytes.
inline constexpr std::size_t kMaxProjectIdLength = 128;

/// Check a project id: non-empty, at most `kMaxProjectIdLength` bytes, no
/// `/` (the secret-key separator) and no control characters.
///
/// @return void, or `ReqloomError` (`SchemaInvalid`) naming the problem.
[[nodiscard]] std::expected<void, ReqloomError> validateProjectId(std::string_view id);

/// A fresh random project id (UUID v4 text, from `std::random_device`).
[[nodiscard]] std::string generateProjectId();

/// The secret-store key for `name` in `project`: `<project id>/<name>`.
///
/// @return The key, or `ReqloomError` (`SchemaInvalid`) when the project has
///         no valid id; the detail tells the user to add `id:` to reqloom.yaml.
[[nodiscard]] std::expected<std::string, ReqloomError> projectSecretKey(const Project& project,
                                                                        std::string_view name);

}  // namespace reqloom::engine
