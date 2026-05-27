#pragma once

#include <QtCore/QStringList>

namespace chainapi::cli {

/// Execute a single operation chain.
int runCommand(const QStringList& args);

}  // namespace chainapi::cli
