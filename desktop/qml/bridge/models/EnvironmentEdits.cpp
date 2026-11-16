// EnvironmentEdits — see header.
#include "EnvironmentEdits.h"

#include <QtCore/QString>

#include <map>

namespace reqloom::desktop::qml {

namespace {

[[nodiscard]] std::map<QString, QString> storedVariables(
    const std::vector<std::pair<QString, QString>>& rows) {
    std::map<QString, QString> out;
    for (const auto& [key, value] : rows) {
        if (!key.isEmpty() && key != QStringLiteral("baseUrl")) {
            out.insert_or_assign(key, value);
        }
    }
    return out;
}

}  // namespace

bool hasUnsavedEnvironmentEdits(const EnvironmentEdits& baseline, const EnvironmentEdits& live) {
    return baseline.name.trimmed() != live.name.trimmed() ||
           baseline.baseUrl.trimmed() != live.baseUrl.trimmed() ||
           storedVariables(baseline.variables) != storedVariables(live.variables);
}

}  // namespace reqloom::desktop::qml
