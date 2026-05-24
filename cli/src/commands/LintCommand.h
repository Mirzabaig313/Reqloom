#pragma once

#include <QtCore/QStringList>

namespace chainapi::cli {

/// Validate the project schema. PRD FR-13.6.
int lintCommand(const QStringList& args);

}  // namespace chainapi::cli
