// `reqloom show <run> <step>` (PRD FR-13.13): what one step of an earlier `reqloom run` sent and
// received, read back from the project's run history with the same masking as `run`'s output.
#pragma once

#include <QtCore/QStringList>

namespace reqloom::cli {

/// Exit code per `ExitCodes.h`.
[[nodiscard]] int showCommand(const QStringList& args);

}  // namespace reqloom::cli
