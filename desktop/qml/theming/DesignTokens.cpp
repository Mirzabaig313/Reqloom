// DesignTokens — see header.
#include "DesignTokens.h"
#include "../bridge/ThemeController.h"

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
    return &instance;
}

bool DesignTokens::isDark() const {
    auto* ctrl = ThemeController::create(nullptr, nullptr);
    return ctrl->isDark();
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
