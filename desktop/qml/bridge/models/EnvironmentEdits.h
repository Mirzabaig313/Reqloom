// EnvironmentEdits — pure rule for when the Manage Environment dialog holds unsaved changes.
#pragma once

#include <QtCore/QString>

#include <utility>
#include <vector>

namespace reqloom::desktop::qml {

/// One environment as the dialog edits it: its name, base URL and variable rows.
struct EnvironmentEdits {
    QString name;
    QString baseUrl;
    std::vector<std::pair<QString, QString>> variables;
};

/// True when saving `live` would store something other than `baseline`. Compared as the save
/// stores them: name and base URL trimmed, variables as a map that drops rows with an empty key
/// and a "baseUrl" row, so row order and blank rows never count as changes.
[[nodiscard]] bool hasUnsavedEnvironmentEdits(const EnvironmentEdits& baseline,
                                              const EnvironmentEdits& live);

}  // namespace reqloom::desktop::qml
