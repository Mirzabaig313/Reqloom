// ThemeController — QML-facing singleton that owns the appearance mode
// (Light/Dark/System) and density, and persists them via QSettings (mirroring
// the Widgets ThemeManager). DesignTokens reads the resolved appearance from
// this singleton to return the correct palette. QML binds to its properties
// and gets repainted on mode change via NOTIFY.
#pragma once

#include "../../src/theming/Theme.h"

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtGui/QGuiApplication>
#include <QtGui/QStyleHints>
#include <QtQml/qqmlregistration.h>

#include <cstdint>

class QQmlEngine;
class QJSEngine;

namespace reqloom::desktop::qml {

class ThemeController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QString mode READ mode WRITE setMode NOTIFY modeChanged)
    Q_PROPERTY(QString density READ density WRITE setDensity NOTIFY densityChanged)
    Q_PROPERTY(bool isDark READ isDark NOTIFY modeChanged)

public:
    ~ThemeController() override;

    ThemeController(const ThemeController&) = delete;
    ThemeController& operator=(const ThemeController&) = delete;
    ThemeController(ThemeController&&) = delete;
    ThemeController& operator=(ThemeController&&) = delete;

    static ThemeController* create(QQmlEngine*, QJSEngine*);

    [[nodiscard]] QString mode() const { return mode_; }
    [[nodiscard]] QString density() const { return density_; }
    [[nodiscard]] bool isDark() const;

    void setMode(const QString& mode);
    void setDensity(const QString& density);

    /// Resolved appearance for the current mode (Light or Dark).
    [[nodiscard]] theming::Appearance resolvedAppearance() const;

signals:
    void modeChanged();
    void densityChanged();

private:
    explicit ThemeController(QObject* parent = nullptr);

    void loadSettings();
    void saveSettings() const;

    QString mode_{QStringLiteral("system")};    ///< "light" / "dark" / "system"
    QString density_{QStringLiteral("comfortable")};  ///< "comfortable" / "compact"
};

}  // namespace reqloom::desktop::qml
