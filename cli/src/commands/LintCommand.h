// `reqloom lint`: validate a project's schema and dry-run every operation's chain.
#pragma once

#include <QtCore/QStringList>

namespace reqloom::cli {

/// Validate the project schema; the exit code follows `ExitCodes.h`.
[[nodiscard]] int lintCommand(const QStringList& args);

}  // namespace reqloom::cli
