// ThemeManager — see header. Applies and persists the appearance preference.
#include "ThemeManager.h"

#include <QtCore/QSettings>
#include <QtGui/QGuiApplication>
#include <QtGui/QStyleHints>
#include <QtWidgets/QApplication>

namespace chainapi::desktop::theming {

namespace {

constexpr auto kSettingsKey = "appearance/mode";

[[nodiscard]] QString modeToString(ThemeManager::Mode mode) {
    switch (mode) {
        case ThemeManager::Mode::Light:
            return QStringLiteral("light");
        case ThemeManager::Mode::Dark:
            return QStringLiteral("dark");
        case ThemeManager::Mode::System:
            return QStringLiteral("system");
    }
    return QStringLiteral("system");
}

[[nodiscard]] ThemeManager::Mode modeFromString(const QString& value) {
    if (value == QStringLiteral("light")) {
        return ThemeManager::Mode::Light;
    }
    if (value == QStringLiteral("dark")) {
        return ThemeManager::Mode::Dark;
    }
    return ThemeManager::Mode::System;
}

}  // namespace

ThemeManager::ThemeManager(QObject* parent) : QObject(parent) {}

ThemeManager::~ThemeManager() = default;

void ThemeManager::start() {
    QSettings settings;
    mode_ = modeFromString(settings.value(QString::fromUtf8(kSettingsKey)).toString());

    // Follow live OS appearance changes while in System mode (Qt 6.5+). The
    // lambda has a receiver (this) so it auto-disconnects on destruction.
    connect(QGuiApplication::styleHints(),
            &QStyleHints::colorSchemeChanged,
            this,
            [this](Qt::ColorScheme /*scheme*/) {
                if (mode_ == Mode::System) {
                    apply();
                }
            });

    apply();
}

void ThemeManager::setMode(Mode mode) {
    if (mode_ == mode) {
        return;
    }
    mode_ = mode;
    QSettings settings;
    settings.setValue(QString::fromUtf8(kSettingsKey), modeToString(mode_));
    apply();
}

Appearance ThemeManager::effectiveAppearance() const {
    switch (mode_) {
        case Mode::Light:
            return Appearance::Light;
        case Mode::Dark:
            return Appearance::Dark;
        case Mode::System: {
            const auto scheme = QGuiApplication::styleHints()->colorScheme();
            return scheme == Qt::ColorScheme::Light ? Appearance::Light : Appearance::Dark;
        }
    }
    return Appearance::Dark;
}

void ThemeManager::apply() {
    theme_ = Theme::resolve(effectiveAppearance());
    if (auto* app = qobject_cast<QApplication*>(QApplication::instance())) {
        app->setStyleSheet(theme_.styleSheet());
    }
    emit themeChanged(theme_);
}

}  // namespace chainapi::desktop::theming
