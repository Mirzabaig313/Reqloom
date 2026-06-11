// DesignTokens — see header.
#include "DesignTokens.h"
#include "../bridge/ThemeController.h"

#include <QtQml/QQmlEngine>

namespace reqloom::desktop::qml {

DesignTokens::DesignTokens(QObject* parent) : QObject(parent) {
    // ThemeController is a static singleton; connect to its modeChanged so
    // we re-resolve the palette and broadcast tokensChanged to all QML bindings.
    auto* ctrl = ThemeController::create(nullptr, nullptr);
    connect(ctrl, &ThemeController::modeChanged, this, &DesignTokens::onModeChanged);
    onModeChanged();  // seed the initial palette
}

DesignTokens::~DesignTokens() = default;

DesignTokens* DesignTokens::create(QQmlEngine*, QJSEngine*) {
    // Single-TU instance (see ThemeController::create for the rationale): an
    // inline header definition emitted one static per translation unit.
    static DesignTokens instance;
    // CppOwnership: the QML engine must not delete this static-storage
    // singleton at teardown (see ThemeController::create).
    QQmlEngine::setObjectOwnership(&instance, QQmlEngine::CppOwnership);
    return &instance;
}

bool DesignTokens::isDark() const {
    auto* ctrl = ThemeController::create(nullptr, nullptr);
    return ctrl->isDark();
}

QColor DesignTokens::shadow() const {
    // Soft elevation shadow tinted toward the tidepool graphite identity.
    if (isDark()) {
        return QColor::fromRgbF(0.0, 0.02, 0.03, 0.55);
    }
    return QColor::fromRgbF(0.04, 0.09, 0.10, 0.16);
}

QColor DesignTokens::glassFill() const {
    // Translucent panel fill so the blurred iridescent backdrop reads through
    // the glass. Lower alpha in dark (deeper frost), higher in light (cleaner).
    QColor c = p().surfaceRaised;
    c.setAlphaF(isDark() ? 0.58F : 0.66F);
    return c;
}

QColor DesignTokens::glassBorder() const {
    // A faint light highlight along the glass edge (the wet-shell rim).
    return isDark() ? QColor::fromRgbF(1.0, 1.0, 1.0, 0.10) : QColor::fromRgbF(1.0, 1.0, 1.0, 0.55);
}

QColor DesignTokens::canvasTop() const {
    return isDark() ? QColor::fromRgbF(0.07, 0.16, 0.18, 1.0)
                    : QColor::fromRgbF(0.90, 0.95, 0.96, 1.0);
}

QColor DesignTokens::canvasBottom() const {
    return isDark() ? QColor::fromRgbF(0.05, 0.10, 0.12, 1.0)
                    : QColor::fromRgbF(0.95, 0.93, 0.95, 1.0);
}

QColor DesignTokens::glowTeal() const {
    return isDark() ? QColor::fromRgbF(0.05, 0.65, 0.62, 0.55)
                    : QColor::fromRgbF(0.05, 0.65, 0.62, 0.40);
}

QColor DesignTokens::glowSeafoam() const {
    return isDark() ? QColor::fromRgbF(0.43, 0.82, 0.77, 0.45)
                    : QColor::fromRgbF(0.43, 0.82, 0.77, 0.40);
}

QColor DesignTokens::glowBlush() const {
    return isDark() ? QColor::fromRgbF(0.96, 0.72, 0.76, 0.32)
                    : QColor::fromRgbF(0.96, 0.72, 0.76, 0.42);
}

void DesignTokens::onModeChanged() {
    auto* ctrl = ThemeController::create(nullptr, nullptr);
    theme_ = theming::Theme::resolve(ctrl->resolvedAppearance());
    emit tokensChanged();
}

QColor DesignTokens::methodColor(const QString& method) const {
    if (method == QLatin1String("GET")) {
        return p().methodGet;
    }
    if (method == QLatin1String("POST")) {
        return p().methodPost;
    }
    if (method == QLatin1String("PUT")) {
        return p().methodPut;
    }
    if (method == QLatin1String("PATCH")) {
        return p().methodPatch;
    }
    if (method == QLatin1String("DELETE")) {
        return p().methodDelete;
    }
    return p().textSecondary;
}

QColor DesignTokens::statusColor(const QString& token) const {
    if (token == QLatin1String("running")) {
        return p().statusRunning;
    }
    if (token == QLatin1String("success")) {
        return p().statusSuccess;
    }
    if (token == QLatin1String("warning")) {
        return p().statusWarning;
    }
    if (token == QLatin1String("error")) {
        return p().statusError;
    }
    if (token == QLatin1String("cancelled")) {
        return p().statusCancelled;
    }
    if (token == QLatin1String("blocked")) {
        return p().statusBlocked;
    }
    if (token == QLatin1String("skipped")) {
        return p().statusIdle;
    }
    return p().statusIdle;
}

QString DesignTokens::statusGlyph(const QString& token) const {
    if (token == QLatin1String("running")) {
        return QStringLiteral("\u25CF");
    }
    if (token == QLatin1String("success")) {
        return QStringLiteral("\u2713");
    }
    if (token == QLatin1String("warning")) {
        return QStringLiteral("\u25B2");
    }
    if (token == QLatin1String("error")) {
        return QStringLiteral("\u2715");
    }
    if (token == QLatin1String("cancelled")) {
        return QStringLiteral("\u2298");
    }
    if (token == QLatin1String("blocked")) {
        return QStringLiteral("\u23F8");
    }
    if (token == QLatin1String("skipped")) {
        return QStringLiteral("\u2013");
    }
    return QStringLiteral("\u25CB");
}

}  // namespace reqloom::desktop::qml
