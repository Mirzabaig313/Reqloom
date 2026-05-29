// Per-project active-environment persistence. Stateless helpers over a
// caller-supplied QSettings: production passes the app's default store,
// tests pass an isolated temp-file-backed one. Keyed by project path so
// each project remembers its own last-selected environment.
#pragma once

#include <QtCore/QString>

class QSettings;

namespace chainapi::desktop {

class EnvironmentSettings {
public:
    /// Persist `env` as the active environment for the project at
    /// `projectKey`. No-op when `projectKey` or `env` is empty.
    static void save(QSettings& settings, const QString& projectKey, const QString& env);

    /// Load the saved environment for `projectKey`, or an empty string
    /// when none is stored (the caller falls back to the project default).
    [[nodiscard]] static QString load(QSettings& settings, const QString& projectKey);
};

}  // namespace chainapi::desktop
