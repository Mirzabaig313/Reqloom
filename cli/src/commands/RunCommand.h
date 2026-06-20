#pragma once

#include <QtCore/QStringList>

namespace reqloom::cli {

/// Execute a single operation chain.
int runCommand(const QStringList& args);

}  // namespace reqloom::cli
