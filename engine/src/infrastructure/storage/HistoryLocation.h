// Where a project's run history lives: one SQLite file per project folder, shared by the desktop
// app and the CLI so a run started at the command line appears in the app (PRD FR-16.3).
#pragma once

#include <reqloom/engine/ErrorCodes.h>

#include <expected>
#include <filesystem>

namespace reqloom::engine {

/// `<data dir>/history/history-<sha1 of the canonical project folder>.db`, where the data dir is
/// the desktop app's (`QStandardPaths::AppDataLocation` for organisation and application
/// "Reqloom": `~/Library/Application Support/Reqloom/Reqloom` on macOS,
/// `$XDG_DATA_HOME/Reqloom/Reqloom` on Linux, `%APPDATA%/Reqloom/Reqloom` on Windows), or the
/// directory `setConfigDirectoryForTesting` names. Creates nothing.
[[nodiscard]] std::expected<std::filesystem::path, ReqloomError> historyDatabasePathFor(
    const std::filesystem::path& projectRoot);

}  // namespace reqloom::engine
