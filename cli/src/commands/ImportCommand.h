#pragma once

#include <QtCore/QStringList>

namespace reqloom::cli {

/// Import an OpenAPI 3.0/3.1 document (YAML or JSON) into a Reqloom project.
[[nodiscard]] int importCommand(const QStringList& args);

}  // namespace reqloom::cli
