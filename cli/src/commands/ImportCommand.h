#pragma once

#include <QtCore/QStringList>

namespace chainapi::cli {

/// Import an external API spec (OpenAPI, Postman, Bruno, Insomnia).
int importCommand(const QStringList& args);

}  // namespace chainapi::cli
