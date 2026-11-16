// `reqloom run`: execute an operation and its prerequisite chain.
#pragma once

#include <QtCore/QStringList>

namespace reqloom::cli {

/// Execute a single operation chain; the exit code follows `ExitCodes.h`, or 130/143 when a
/// SIGINT/SIGTERM cancelled the run.
[[nodiscard]] int runCommand(const QStringList& args);

}  // namespace reqloom::cli
