// EnvironmentSettings — see header. Thin QSettings group accessor.
#include "EnvironmentSettings.h"

#include <QtCore/QSettings>

namespace chainapi::desktop {

namespace {

// All per-project environment selections live under one settings group so
// they're easy to inspect and don't collide with other preferences.
constexpr const char* kGroup = "activeEnvironment";

}  // namespace

void EnvironmentSettings::save(QSettings& settings, const QString& projectKey, const QString& env) {
    if (projectKey.isEmpty() || env.isEmpty()) {
        return;
    }
    settings.beginGroup(QString::fromUtf8(kGroup));
    settings.setValue(projectKey, env);
    settings.endGroup();
    // Flush to backing store immediately. Without this a second QSettings
    // instance opened over the same file (as the round-trip test does) can
    // read stale content on Windows, where the write is otherwise buffered
    // until destruction.
    settings.sync();
}

QString EnvironmentSettings::load(QSettings& settings, const QString& projectKey) {
    if (projectKey.isEmpty()) {
        return QString{};
    }
    settings.beginGroup(QString::fromUtf8(kGroup));
    const QString env = settings.value(projectKey).toString();
    settings.endGroup();
    return env;
}

}  // namespace chainapi::desktop
