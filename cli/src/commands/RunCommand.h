#pragma once

#include <QtCore/QStringList>

namespace chainapi::cli {

/// Execute a single operation chain, rendering output per the user's
/// chosen renderer (text by default, --json or --junit available).
/// Returns 0 on success, non-zero on failure (PRD FR-13.4).
int runCommand(const QStringList& args);

}  // namespace chainapi::cli
