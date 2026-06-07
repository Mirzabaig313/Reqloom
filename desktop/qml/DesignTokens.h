// DesignTokens — the QML-facing design system (replaces the Widgets-era
// Theme.cpp/QSS). A QML singleton exposing semantic colors, spacing, and radii
// so every .qml reads `DesignTokens.surfaceBase` etc. instead of raw literals.
// Dark indigo-violet palette per DESIGN.md §2 (accent hue ~285).
#pragma once

#include <QtQml/qqmlregistration.h>
#include <QtGui/QColor>

#include <QtCore/QObject>

class QQmlEngine;
class QJSEngine;

namespace reqloom::desktop::qml {

class DesignTokens : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // Surfaces
    Q_PROPERTY(QColor surfaceBase READ surfaceBase CONSTANT)
    Q_PROPERTY(QColor surfaceRaised READ surfaceRaised CONSTANT)
    Q_PROPERTY(QColor surfaceSunken READ surfaceSunken CONSTANT)
    Q_PROPERTY(QColor borderSubtle READ borderSubtle CONSTANT)
    Q_PROPERTY(QColor borderStrong READ borderStrong CONSTANT)
    // Text
    Q_PROPERTY(QColor textPrimary READ textPrimary CONSTANT)
    Q_PROPERTY(QColor textSecondary READ textSecondary CONSTANT)
    Q_PROPERTY(QColor textInverse READ textInverse CONSTANT)
    // Accent
    Q_PROPERTY(QColor accent READ accent CONSTANT)
    Q_PROPERTY(QColor accentHover READ accentHover CONSTANT)
    Q_PROPERTY(QColor accentMuted READ accentMuted CONSTANT)
    // HTTP method hues (DESIGN.md §6.2a)
    Q_PROPERTY(QColor methodGet READ methodGet CONSTANT)
    Q_PROPERTY(QColor methodPost READ methodPost CONSTANT)
    Q_PROPERTY(QColor methodPut READ methodPut CONSTANT)
    Q_PROPERTY(QColor methodPatch READ methodPatch CONSTANT)
    Q_PROPERTY(QColor methodDelete READ methodDelete CONSTANT)
    // Spacing scale (DESIGN.md §5.1), in device-independent px
    Q_PROPERTY(int spaceXs READ spaceXs CONSTANT)
    Q_PROPERTY(int spaceSm READ spaceSm CONSTANT)
    Q_PROPERTY(int spaceMd READ spaceMd CONSTANT)
    Q_PROPERTY(int spaceLg READ spaceLg CONSTANT)
    Q_PROPERTY(int spaceXl READ spaceXl CONSTANT)
    Q_PROPERTY(int radius READ radius CONSTANT)
    Q_PROPERTY(int radiusSm READ radiusSm CONSTANT)

public:
    explicit DesignTokens(QObject* parent = nullptr) : QObject(parent) {}

    static DesignTokens* create(QQmlEngine*, QJSEngine*) { return new DesignTokens; }

    [[nodiscard]] QColor surfaceBase() const { return QColor(QStringLiteral("#0F1117")); }
    [[nodiscard]] QColor surfaceRaised() const { return QColor(QStringLiteral("#161A22")); }
    [[nodiscard]] QColor surfaceSunken() const { return QColor(QStringLiteral("#0B0D12")); }
    [[nodiscard]] QColor borderSubtle() const { return QColor(QStringLiteral("#252A36")); }
    [[nodiscard]] QColor borderStrong() const { return QColor(QStringLiteral("#39414F")); }

    [[nodiscard]] QColor textPrimary() const { return QColor(QStringLiteral("#E6EAF2")); }
    [[nodiscard]] QColor textSecondary() const { return QColor(QStringLiteral("#9AA4B8")); }
    [[nodiscard]] QColor textInverse() const { return QColor(QStringLiteral("#0F1117")); }

    [[nodiscard]] QColor accent() const { return QColor(QStringLiteral("#8B7FFF")); }
    [[nodiscard]] QColor accentHover() const { return QColor(QStringLiteral("#A79DFF")); }
    [[nodiscard]] QColor accentMuted() const { return QColor(QStringLiteral("#2A2748")); }

    [[nodiscard]] QColor methodGet() const { return QColor(QStringLiteral("#5B9BFF")); }
    [[nodiscard]] QColor methodPost() const { return QColor(QStringLiteral("#4ECB8E")); }
    [[nodiscard]] QColor methodPut() const { return QColor(QStringLiteral("#E0913A")); }
    [[nodiscard]] QColor methodPatch() const { return QColor(QStringLiteral("#E0C53A")); }
    [[nodiscard]] QColor methodDelete() const { return QColor(QStringLiteral("#F06A6A")); }

    [[nodiscard]] int spaceXs() const { return 4; }
    [[nodiscard]] int spaceSm() const { return 8; }
    [[nodiscard]] int spaceMd() const { return 12; }
    [[nodiscard]] int spaceLg() const { return 20; }
    [[nodiscard]] int spaceXl() const { return 32; }
    [[nodiscard]] int radius() const { return 10; }
    [[nodiscard]] int radiusSm() const { return 6; }
};

}  // namespace reqloom::desktop::qml
