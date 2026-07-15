// ThemeController — see header.
#include "ThemeController.h"

#include <QtCore/QSettings>
#include <QtQml/QQmlEngine>

namespace reqloom::desktop::qml {

namespace {

constexpr auto kModeKey = "appearance/mode";
constexpr auto kDensityKey = "appearance/density";

[[nodiscard]] QString normalizedDensity(const QString& density) {
    return density.toLower().trimmed() == QLatin1String("compact") ? QStringLiteral("compact")
                                                                   : QStringLiteral("comfortable");
}

}  // namespace

ThemeController::ThemeController(QObject* parent) : QObject(parent) {
    loadSettings();
    // Follow live OS appearance changes when in System mode.
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
    // The instance lives in this single translation unit so QML and every C++
    // consumer (e.g. DesignTokens) share one object. Defining it inline in the
    // header emitted a separate function-local static per TU that failed to
    // merge across the static-library boundary — two singletons, so theme
    // changes on the QML instance never reached the DesignTokens-bound one.
    static ThemeController instance;
    // The QML engine deletes singleton instances at teardown; this one has
    // static storage (shared across QML + C++ consumers), so hand the engine
    // CppOwnership to stop it from calling delete on non-heap memory.
    QQmlEngine::setObjectOwnership(&instance, QQmlEngine::CppOwnership);
    return &instance;
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
    // System: follow the platform.
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
