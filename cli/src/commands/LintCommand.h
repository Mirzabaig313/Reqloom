#pragma once

#include <QtCore/QStringList>

namespace reqloom::cli {

/// Validate the project schema.
int lintCommand(const QStringList& args);

}  // namespace reqloom::cli
