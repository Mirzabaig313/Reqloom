// DesignTokens — appearance-aware QML design-system singleton .
// Replaces the old QSS Theme.cpp for QML. All properties are NOTIFY-driven —
// when ThemeController changes mode the whole QML tree repaints via bindings.
// Token values are sourced from Theme.cpp's two OKLCH palettes (reusing the
// same Color/oklch pipeline) so light and dark match the Widgets app exactly
// during the migration. Spacing matches Theme::space (Lg=16, Xl=24, Xxl=32).
#pragma once

#include "../../src/theming/Theme.h"

#include <QtQml/qqmlregistration.h>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtGui/QColor>

class QEvent;
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
    Q_PROPERTY(QColor surfaceOverlay READ surfaceOverlay NOTIFY tokensChanged)
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
    // HTTP method hues
    Q_PROPERTY(QColor methodGet READ methodGet NOTIFY tokensChanged)
    Q_PROPERTY(QColor methodPost READ methodPost NOTIFY tokensChanged)
    Q_PROPERTY(QColor methodPut READ methodPut NOTIFY tokensChanged)
    Q_PROPERTY(QColor methodPatch READ methodPatch NOTIFY tokensChanged)
    Q_PROPERTY(QColor methodDelete READ methodDelete NOTIFY tokensChanged)
    // Status palette
    Q_PROPERTY(QColor statusIdle READ statusIdle NOTIFY tokensChanged)
    Q_PROPERTY(QColor statusRunning READ statusRunning NOTIFY tokensChanged)
    Q_PROPERTY(QColor statusSuccess READ statusSuccess NOTIFY tokensChanged)
    Q_PROPERTY(QColor statusWarning READ statusWarning NOTIFY tokensChanged)
    Q_PROPERTY(QColor statusError READ statusError NOTIFY tokensChanged)
    Q_PROPERTY(QColor statusCancelled READ statusCancelled NOTIFY tokensChanged)
    Q_PROPERTY(QColor statusBlocked READ statusBlocked NOTIFY tokensChanged)
    Q_PROPERTY(QColor statusSkipped READ statusSkipped NOTIFY tokensChanged)
    // Spacing scale matching Theme::space
    // T-D1 decision: use Theme.cpp's scale (Lg=16/Xl=24) not the old QML
    // scale (Lg=20/Xl=32) so tokens stay consistent with the Widgets app.
    Q_PROPERTY(int spaceXs READ spaceXs CONSTANT)
    Q_PROPERTY(int spaceSm READ spaceSm NOTIFY metricsChanged)
    Q_PROPERTY(int spaceMd READ spaceMd NOTIFY metricsChanged)
    Q_PROPERTY(int spaceLg READ spaceLg NOTIFY metricsChanged)
    Q_PROPERTY(int spaceXl READ spaceXl NOTIFY metricsChanged)
    Q_PROPERTY(int spaceXxl READ spaceXxl NOTIFY metricsChanged)
    Q_PROPERTY(int radius READ radius CONSTANT)
    Q_PROPERTY(int radiusSm READ radiusSm CONSTANT)
    Q_PROPERTY(int radiusLg READ radiusLg CONSTANT)
    Q_PROPERTY(int radiusPill READ radiusPill CONSTANT)
    // Typography — application-derived point roles for QML.
    Q_PROPERTY(QString fontSans READ fontSans NOTIFY typographyChanged)
    Q_PROPERTY(QString fontMono READ fontMono CONSTANT)
    Q_PROPERTY(qreal fontTitlePointSize READ fontTitlePointSize NOTIFY typographyChanged)
    Q_PROPERTY(qreal fontSubtitlePointSize READ fontSubtitlePointSize NOTIFY typographyChanged)
    Q_PROPERTY(qreal fontBodyPointSize READ fontBodyPointSize NOTIFY typographyChanged)
    Q_PROPERTY(qreal fontLabelPointSize READ fontLabelPointSize NOTIFY typographyChanged)
    Q_PROPERTY(qreal fontCaptionPointSize READ fontCaptionPointSize NOTIFY typographyChanged)
    Q_PROPERTY(qreal fontMonoPointSize READ fontMonoPointSize NOTIFY typographyChanged)
    Q_PROPERTY(int weightRegular READ weightRegular CONSTANT)
    Q_PROPERTY(int weightMedium READ weightMedium CONSTANT)
    Q_PROPERTY(int weightSemiBold READ weightSemiBold CONSTANT)
    Q_PROPERTY(int weightBold READ weightBold CONSTANT)
    // Control sizing.
    Q_PROPERTY(int controlHeight READ controlHeight NOTIFY metricsChanged)
    Q_PROPERTY(int controlHeightLg READ controlHeightLg NOTIFY metricsChanged)
    Q_PROPERTY(int iconSize READ iconSize CONSTANT)
    // Motion — one canonical spring tuning (critically-ish damped harmonic
    // oscillator) so position/size settling reads as physical and coherent
    // app-wide. `motionSpring`/`motionDamping`/`motionMass`/`motionEpsilon`
    // feed Qt SpringAnimation; the two durations cover color/opacity fades
    // where a spring doesn't apply. Tuned for a lively settle with a hint of
    // overshoot (never robotic, never bouncy).
    Q_PROPERTY(qreal motionSpring READ motionSpring CONSTANT)
    Q_PROPERTY(qreal motionDamping READ motionDamping CONSTANT)
    Q_PROPERTY(qreal motionMass READ motionMass CONSTANT)
    Q_PROPERTY(qreal motionEpsilon READ motionEpsilon CONSTANT)
    Q_PROPERTY(int motionFast READ motionFast CONSTANT)
    Q_PROPERTY(int motionMedium READ motionMedium CONSTANT)
    Q_PROPERTY(qreal phi READ phi CONSTANT)
    // Soft elevation shadow colour (translucent), appearance-aware.
    Q_PROPERTY(QColor shadow READ shadow NOTIFY tokensChanged)
    // Glassmorphism: translucent panel fill + highlight border, the iridescent
    // backdrop gradient stops, and three nacre glow-blob colours.
    Q_PROPERTY(QColor glassFill READ glassFill NOTIFY tokensChanged)
    Q_PROPERTY(QColor glassBorder READ glassBorder NOTIFY tokensChanged)
    Q_PROPERTY(QColor canvasTop READ canvasTop NOTIFY tokensChanged)
    Q_PROPERTY(QColor canvasBottom READ canvasBottom NOTIFY tokensChanged)
    Q_PROPERTY(QColor glowTeal READ glowTeal NOTIFY tokensChanged)
    Q_PROPERTY(QColor glowSeafoam READ glowSeafoam NOTIFY tokensChanged)
    Q_PROPERTY(QColor glowBlush READ glowBlush NOTIFY tokensChanged)
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
    [[nodiscard]] QColor surfaceOverlay() const { return p().surfaceOverlay; }
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
    [[nodiscard]] int spaceSm() const { return compactDensity_ ? 6 : 8; }
    [[nodiscard]] int spaceMd() const { return compactDensity_ ? 8 : 12; }
    [[nodiscard]] int spaceLg() const { return compactDensity_ ? 12 : 16; }
    [[nodiscard]] int spaceXl() const { return compactDensity_ ? 16 : 24; }
    [[nodiscard]] int spaceXxl() const { return compactDensity_ ? 24 : 32; }
    [[nodiscard]] int radius() const { return 8; }
    [[nodiscard]] int radiusSm() const { return 6; }
    [[nodiscard]] int radiusLg() const { return 12; }
    [[nodiscard]] int radiusPill() const { return 999; }
    // Typography. Point-size roles follow the application font so OS scale
    // changes propagate without coupling type to density or window width.
    [[nodiscard]] QString fontSans() const;
    [[nodiscard]] QString fontMono() const { return QStringLiteral("Geist Mono"); }
    [[nodiscard]] qreal fontTitlePointSize() const { return fontBasePointSize() * 1.30; }
    [[nodiscard]] qreal fontSubtitlePointSize() const { return fontBasePointSize() * 1.15; }
    [[nodiscard]] qreal fontBodyPointSize() const { return fontBasePointSize(); }
    [[nodiscard]] qreal fontLabelPointSize() const { return fontBasePointSize() * 0.92; }
    [[nodiscard]] qreal fontCaptionPointSize() const { return fontBasePointSize() * 0.85; }
    [[nodiscard]] qreal fontMonoPointSize() const { return fontBasePointSize(); }
    [[nodiscard]] int weightRegular() const { return 400; }
    [[nodiscard]] int weightMedium() const { return 500; }
    [[nodiscard]] int weightSemiBold() const { return 600; }
    [[nodiscard]] int weightBold() const { return 700; }
    [[nodiscard]] int controlHeight() const { return compactDensity_ ? 30 : 34; }
    [[nodiscard]] int controlHeightLg() const { return compactDensity_ ? 32 : 36; }
    [[nodiscard]] int iconSize() const { return 16; }
    // Motion tuning — see the Q_PROPERTY block. Qt's SpringAnimation `damping`
    // is normalised 0..1 (1 ≈ critically damped); 0.32 leaves a small, organic
    // overshoot. `epsilon` is the settle threshold in px.
    [[nodiscard]] qreal motionSpring() const { return 3.2; }
    [[nodiscard]] qreal motionDamping() const { return 0.32; }
    [[nodiscard]] qreal motionMass() const { return 1.0; }
    [[nodiscard]] qreal motionEpsilon() const { return 0.25; }
    [[nodiscard]] int motionFast() const { return 120; }
    [[nodiscard]] int motionMedium() const { return 200; }
    // Golden ratio (φ). Used for true proportions the eye compares side by
    // side — e.g. the editor:response pane split — NOT the spacing scale,
    // which stays on the 4px grid for pixel-crisp rendering.
    [[nodiscard]] qreal phi() const { return 1.6180339887; }
    [[nodiscard]] QColor shadow() const;
    [[nodiscard]] QColor glassFill() const;
    [[nodiscard]] QColor glassBorder() const;
    [[nodiscard]] QColor canvasTop() const;
    [[nodiscard]] QColor canvasBottom() const;
    [[nodiscard]] QColor glowTeal() const;
    [[nodiscard]] QColor glowSeafoam() const;
    [[nodiscard]] QColor glowBlush() const;
    [[nodiscard]] bool isDark() const;

    /// HTTP method hue — single QML source (dedupes MethodBadge/ChainView).
    Q_INVOKABLE [[nodiscard]] QColor methodColor(const QString& method) const;
    /// Status token → resolved colour (matches TimelineModel token strings).
    Q_INVOKABLE [[nodiscard]] QColor statusColor(const QString& token) const;
    /// Status token → glyph (colour+glyph pair per .
    Q_INVOKABLE [[nodiscard]] QString statusGlyph(const QString& token) const;

signals:
    void tokensChanged();
    void metricsChanged();
    void typographyChanged();

private:
    /// Returns the resolved palette for the current appearance.
    [[nodiscard]] const theming::Palette& p() const noexcept { return theme_.palette(); }
    [[nodiscard]] qreal fontBasePointSize() const;
    bool eventFilter(QObject* watched, QEvent* event) override;

    void onModeChanged();
    void onDensityChanged();

    theming::Theme theme_{theming::Theme::resolve(theming::Appearance::Dark)};
    bool compactDensity_{};
};

}  // namespace reqloom::desktop::qml
