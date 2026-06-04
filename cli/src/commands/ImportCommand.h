#pragma once

#include <QtCore/QStringList>

namespace reqloom::cli {

/// Import an external API spec (OpenAPI, Postman, Bruno, Insomnia).
int importCommand(const QStringList& args);

}  // namespace reqloom::cli
