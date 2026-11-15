// Project identity: the stable `id:` that namespaces a project's keychain
// secrets so one project cannot read another's.
#pragma once

#include <reqloom/engine/ErrorCodes.h>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
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

class ProjectTrustStore;

/// Windows spelling rule for two folder paths given as UTF-8 text: `/` and `\` are the same
/// separator, trailing separators are ignored (except a drive root's) and ASCII letters
/// compare case-insensitively; other bytes compare exactly. Used only when one of the folders
/// no longer exists, so the file system cannot decide.
[[nodiscard]] bool windowsFolderPathsEqual(std::string_view a, std::string_view b);

/// Gate every secret read or write for `project`: it needs a valid id and a folder (`rootDir`), and
/// that folder must be the one `trust` records for the id (same directory on disk, or the same
/// normalised path when the recorded one no longer exists). An id with no record is bound to this
/// folder now (trust on first use).
///
/// @return void when secrets may be used; otherwise `ReqloomError`: `SchemaInvalid` for a
///         missing/invalid id, `SecretAccessFailed` for no folder, no store, a store failure,
///         or an id recorded for a different folder.
[[nodiscard]] std::expected<void, ReqloomError> checkProjectFolder(ProjectTrustStore* trust,
                                                                   const Project& project);

/// The other folder `trust` records for `project`'s id when it differs from the project's
/// own, else an empty optional. Read-only.
[[nodiscard]] std::optional<std::filesystem::path> projectFolderConflict(ProjectTrustStore& trust,
                                                                         const Project& project);

/// Explicit user action after moving a project: bind its id to its current
/// folder, replacing any earlier record.
[[nodiscard]] std::expected<void, ReqloomError> rebindProjectFolder(ProjectTrustStore& trust,
                                                                    const Project& project);

/// Insert one top-level `id: <id>` line into reqloom.yaml text, touching nothing else: after
/// the top-level `name:` entry if any, else before the first top-level key. Keeps line
/// endings, trailing newline and a UTF-8 BOM.
///
/// @return The edited text; an empty optional when the root already has an `id:` key; or
///         `ReqloomError` (`SchemaInvalid`) when the text cannot be edited safely (flow-style
///         or non-mapping root, several documents, no top-level key, or a non-scalar id).
[[nodiscard]] std::expected<std::optional<std::string>, ReqloomError> insertProjectIdLine(
    std::string_view yamlText, std::string_view id);

}  // namespace reqloom::engine
