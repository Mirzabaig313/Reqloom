// EnvironmentSettings — see header. Thin QSettings group accessor.
#include "EnvironmentSettings.h"

#include <QtCore/QSettings>

namespace reqloom::desktop {

namespace {

// All environment selections live under one settings group.
constexpr const char* kGroup = "activeEnvironment";

// QSettings treats '/' as a key-path separator, so replace it with '|' to keep project keys flat.
QString sanitizeKey(const QString& projectKey) {
    QString k = projectKey;
    k.replace(QLatin1Char('/'), QLatin1Char('|'));
    return k;
}

}  // namespace

void EnvironmentSettings::save(QSettings& settings, const QString& projectKey, const QString& env) {
    if (projectKey.isEmpty() || env.isEmpty()) {
        return;
    }
    settings.beginGroup(QString::fromUtf8(kGroup));
    settings.setValue(sanitizeKey(projectKey), env);
    settings.endGroup();
    // Flush now so a second QSettings over the same file can't read stale content on Windows.
    settings.sync();
}

QString EnvironmentSettings::load(QSettings& settings, const QString& projectKey) {
    if (projectKey.isEmpty()) {
        return QString{};
    }
    settings.beginGroup(QString::fromUtf8(kGroup));
    const QString env = settings.value(sanitizeKey(projectKey)).toString();
    settings.endGroup();
    return env;
}

}  // namespace reqloom::desktop
