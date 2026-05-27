#pragma once

#include <QtCore/QStringList>

namespace chainapi::cli {

/// Validate the project schema.
int lintCommand(const QStringList& args);

}  // namespace chainapi::cli
