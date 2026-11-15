// ThemeController — see header.
#include "ThemeController.h"

#include <QtCore/QSettings>
#include <QtQml/QQmlEngine>

namespace reqloom::desktop::qml {

namespace {

constexpr auto kModeKey = "appearance/mode";
constexpr auto kDensityKey = "appearance/density";
constexpr auto kLegacyDensityKey = "layout/density";

[[nodiscard]] QString normalizedDensity(const QString& density) {
    return density.toLower().trimmed() == QLatin1String("compact") ? QStringLiteral("compact")
                                                                   : QStringLiteral("comfortable");
}

}  // namespace

ThemeController::ThemeController(QObject* parent) : QObject(parent) {
    loadSettings();
    connect(QGuiApplication::styleHints(),
            &QStyleHints::colorSchemeChanged,
            this,
            [this](Qt::ColorScheme /*scheme*/) {
                if (mode_ == QLatin1String("system")) {
                    emit modeChanged();
                }
            });
}

ThemeController::~ThemeController() = default;

ThemeController* ThemeController::create(QQmlEngine*, QJSEngine*) {
    // Defined in one TU: an inline header definition emitted a function-local static per TU that
    // didn't merge across the static-library boundary, giving two singletons.
    static ThemeController instance;
    // CppOwnership: static storage must not be deleted by the QML engine at teardown.
    QQmlEngine::setObjectOwnership(&instance, QQmlEngine::CppOwnership);
    return &instance;
}

void ThemeController::migrateLegacyDensity(QSettings& settings) {
    const QString canonicalKey{QString::fromUtf8(kDensityKey)};
    const QString legacyKey{QString::fromUtf8(kLegacyDensityKey)};
    if (settings.contains(canonicalKey)) {
        settings.setValue(canonicalKey, normalizedDensity(settings.value(canonicalKey).toString()));
        settings.remove(legacyKey);
        return;
    }
    if (settings.contains(legacyKey)) {
        settings.setValue(canonicalKey, normalizedDensity(settings.value(legacyKey).toString()));
        settings.remove(legacyKey);
    }
}

bool ThemeController::isDark() const {
    return resolvedAppearance() == theming::Appearance::Dark;
}

theming::Appearance ThemeController::resolvedAppearance() const {
    if (mode_ == QLatin1String("light")) {
        return theming::Appearance::Light;
    }
    if (mode_ == QLatin1String("dark")) {
        return theming::Appearance::Dark;
    }
    const auto scheme = QGuiApplication::styleHints()->colorScheme();
    return (scheme == Qt::ColorScheme::Light) ? theming::Appearance::Light
                                              : theming::Appearance::Dark;
}

void ThemeController::setMode(const QString& mode) {
    const QString normalized = mode.toLower().trimmed();
    if (normalized == mode_) {
        return;
    }
    mode_ = normalized;
    saveSettings();
    emit modeChanged();
}

void ThemeController::setDensity(const QString& density) {
    const QString normalized{normalizedDensity(density)};
    if (normalized == density_) {
        return;
    }
    density_ = normalized;
    saveSettings();
    emit densityChanged();
}

void ThemeController::loadSettings() {
    QSettings settings;
    migrateLegacyDensity(settings);
    const QString savedMode = settings.value(QString::fromUtf8(kModeKey)).toString();
    if (!savedMode.isEmpty()) {
        mode_ = savedMode;
    }
    density_ = normalizedDensity(settings.value(QString::fromUtf8(kDensityKey)).toString());
}

void ThemeController::saveSettings() const {
    QSettings settings;
    settings.setValue(QString::fromUtf8(kModeKey), mode_);
    settings.setValue(QString::fromUtf8(kDensityKey), density_);
}

}  // namespace reqloom::desktop::qml
