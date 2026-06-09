// DesignTokens — appearance-aware QML design-system singleton (ADR-007, WS-D).
// Replaces the old QSS Theme.cpp for QML. All properties are NOTIFY-driven —
// when ThemeController changes mode the whole QML tree repaints via bindings.
// Token values are sourced from Theme.cpp's two OKLCH palettes (reusing the
// same Color/oklch pipeline) so light and dark match the Widgets app exactly
// during the migration. Spacing matches Theme::space (Lg=16, Xl=24, Xxl=32).
#pragma once

#include "../../src/theming/Theme.h"

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtGui/QColor>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class QJSEngine;

namespace reqloom::desktop::qml {

class ThemeController;

class DesignTokens : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // Surfaces
    Q_PROPERTY(QColor surfaceBase READ surfaceBase NOTIFY tokensChanged)
    Q_PROPERTY(QColor surfaceRaised READ surfaceRaised NOTIFY tokensChanged)
    Q_PROPERTY(QColor surfaceSunken READ surfaceSunken NOTIFY tokensChanged)
    Q_PROPERTY(QColor borderSubtle READ borderSubtle NOTIFY tokensChanged)
    Q_PROPERTY(QColor borderStrong READ borderStrong NOTIFY tokensChanged)
    // Text
    Q_PROPERTY(QColor textPrimary READ textPrimary NOTIFY tokensChanged)
    Q_PROPERTY(QColor textSecondary READ textSecondary NOTIFY tokensChanged)
    Q_PROPERTY(QColor textInverse READ textInverse NOTIFY tokensChanged)
    // Accent
    Q_PROPERTY(QColor accent READ accent NOTIFY tokensChanged)
    Q_PROPERTY(QColor accentHover READ accentHover NOTIFY tokensChanged)
    Q_PROPERTY(QColor accentMuted READ accentMuted NOTIFY tokensChanged)
    // HTTP method hues (DESIGN.md §6.2a)
    Q_PROPERTY(QColor methodGet READ methodGet NOTIFY tokensChanged)
    Q_PROPERTY(QColor methodPost READ methodPost NOTIFY tokensChanged)
    Q_PROPERTY(QColor methodPut READ methodPut NOTIFY tokensChanged)
    Q_PROPERTY(QColor methodPatch READ methodPatch NOTIFY tokensChanged)
    Q_PROPERTY(QColor methodDelete READ methodDelete NOTIFY tokensChanged)
    // Status palette (DESIGN.md §2.5)
    Q_PROPERTY(QColor statusIdle READ statusIdle NOTIFY tokensChanged)
    Q_PROPERTY(QColor statusRunning READ statusRunning NOTIFY tokensChanged)
    Q_PROPERTY(QColor statusSuccess READ statusSuccess NOTIFY tokensChanged)
    Q_PROPERTY(QColor statusWarning READ statusWarning NOTIFY tokensChanged)
    Q_PROPERTY(QColor statusError READ statusError NOTIFY tokensChanged)
    Q_PROPERTY(QColor statusCancelled READ statusCancelled NOTIFY tokensChanged)
    Q_PROPERTY(QColor statusBlocked READ statusBlocked NOTIFY tokensChanged)
    Q_PROPERTY(QColor statusSkipped READ statusSkipped NOTIFY tokensChanged)
    // Spacing scale matching Theme::space (DESIGN.md §5.1).
    // T-D1 decision: use Theme.cpp's scale (Lg=16/Xl=24) not the old QML
    // scale (Lg=20/Xl=32) so tokens stay consistent with the Widgets app.
    Q_PROPERTY(int spaceXs READ spaceXs CONSTANT)
    Q_PROPERTY(int spaceSm READ spaceSm CONSTANT)
    Q_PROPERTY(int spaceMd READ spaceMd CONSTANT)
    Q_PROPERTY(int spaceLg READ spaceLg CONSTANT)
    Q_PROPERTY(int spaceXl READ spaceXl CONSTANT)
    Q_PROPERTY(int spaceXxl READ spaceXxl CONSTANT)
    Q_PROPERTY(int radius READ radius CONSTANT)
    Q_PROPERTY(int radiusSm READ radiusSm CONSTANT)
    // Derived from ThemeController
    Q_PROPERTY(bool isDark READ isDark NOTIFY tokensChanged)

public:
    explicit DesignTokens(QObject* parent = nullptr);
    ~DesignTokens() override;

    static DesignTokens* create(QQmlEngine*, QJSEngine*);

    // Surfaces
    [[nodiscard]] QColor surfaceBase() const { return p().surfaceBase; }
    [[nodiscard]] QColor surfaceRaised() const { return p().surfaceRaised; }
    [[nodiscard]] QColor surfaceSunken() const { return p().surfaceSunken; }
    [[nodiscard]] QColor borderSubtle() const { return p().borderSubtle; }
    [[nodiscard]] QColor borderStrong() const { return p().borderStrong; }
    // Text
    [[nodiscard]] QColor textPrimary() const { return p().textPrimary; }
    [[nodiscard]] QColor textSecondary() const { return p().textSecondary; }
    [[nodiscard]] QColor textInverse() const { return p().textInverse; }
    // Accent
    [[nodiscard]] QColor accent() const { return p().accentBase; }
    [[nodiscard]] QColor accentHover() const { return p().accentHover; }
    [[nodiscard]] QColor accentMuted() const { return p().accentMuted; }
    // Method hues
    [[nodiscard]] QColor methodGet() const { return p().methodGet; }
    [[nodiscard]] QColor methodPost() const { return p().methodPost; }
    [[nodiscard]] QColor methodPut() const { return p().methodPut; }
    [[nodiscard]] QColor methodPatch() const { return p().methodPatch; }
    [[nodiscard]] QColor methodDelete() const { return p().methodDelete; }
    // Status
    [[nodiscard]] QColor statusIdle() const { return p().statusIdle; }
    [[nodiscard]] QColor statusRunning() const { return p().statusRunning; }
    [[nodiscard]] QColor statusSuccess() const { return p().statusSuccess; }
    [[nodiscard]] QColor statusWarning() const { return p().statusWarning; }
    [[nodiscard]] QColor statusError() const { return p().statusError; }
    [[nodiscard]] QColor statusCancelled() const { return p().statusCancelled; }
    [[nodiscard]] QColor statusBlocked() const { return p().statusBlocked; }
    [[nodiscard]] QColor statusSkipped() const { return p().statusIdle; }
    // Spacing (Theme::space scale, T-D1 resolved)
    [[nodiscard]] int spaceXs() const { return 4; }
    [[nodiscard]] int spaceSm() const { return 8; }
    [[nodiscard]] int spaceMd() const { return 12; }
    [[nodiscard]] int spaceLg() const { return 16; }
    [[nodiscard]] int spaceXl() const { return 24; }
    [[nodiscard]] int spaceXxl() const { return 32; }
    [[nodiscard]] int radius() const { return 8; }
    [[nodiscard]] int radiusSm() const { return 5; }
    [[nodiscard]] bool isDark() const;

    /// HTTP method hue — single QML source (dedupes MethodBadge/ChainView).
    Q_INVOKABLE [[nodiscard]] QColor methodColor(const QString& method) const;
    /// Status token → resolved colour (matches TimelineModel token strings).
    Q_INVOKABLE [[nodiscard]] QColor statusColor(const QString& token) const;
    /// Status token → glyph (colour+glyph pair per DESIGN.md §6.1 / a11y).
    Q_INVOKABLE [[nodiscard]] QString statusGlyph(const QString& token) const;

signals:
    void tokensChanged();

private:
    /// Returns the resolved palette for the current appearance.
    [[nodiscard]] const theming::Palette& p() const noexcept { return theme_.palette(); }

    void onModeChanged();

    theming::Theme theme_{theming::Theme::resolve(theming::Appearance::Dark)};
};

}  // namespace reqloom::desktop::qml
