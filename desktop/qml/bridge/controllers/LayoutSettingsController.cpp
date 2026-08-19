// Persists validated fluid shell layout state for QML.
#include "LayoutSettingsController.h"

#include "application/LayoutSettings.h"

#include <QtCore/QMetaType>
#include <QtCore/QSettings>
#include <QtCore/QVariant>
#include <QtQml/QQmlEngine>

namespace reqloom::desktop::qml {

namespace {

constexpr auto kSchemaVersionKey = "layout/schemaVersion";
constexpr auto kExplorerCollapsedKey = "layout/explorerCollapsed";
constexpr auto kResponseCollapsedKey = "layout/responseCollapsed";
constexpr auto kResponseOrientationKey = "layout/responseOrientation";
constexpr auto kSplitterGroup = "splitterSizes";

[[nodiscard]] QString normalizedOrientation(const QString& orientation) {
    const QString normalized{orientation.toLower().trimmed()};
    if (normalized == QLatin1String("horizontal") || normalized == QLatin1String("vertical")) {
        return normalized;
    }
    return QStringLiteral("auto");
}

[[nodiscard]] bool storedBool(const QSettings& settings, const char* key) {
    const QVariant value{settings.value(QString::fromUtf8(key))};
    return value.metaType().id() == QMetaType::Bool && value.toBool();
}

}  // namespace

LayoutSettingsController::LayoutSettingsController(QSettings& settings, QObject* parent)
    : QObject(parent), settings_(settings) {}

LayoutSettingsController* LayoutSettingsController::create(QQmlEngine*, QJSEngine*) {
    static QSettings settings;
    static LayoutSettingsController instance{settings};
    QQmlEngine::setObjectOwnership(&instance, QQmlEngine::CppOwnership);
    return &instance;
}

bool LayoutSettingsController::explorerCollapsed() const {
    return hasCurrentSchema() && storedBool(settings_, kExplorerCollapsedKey);
}

bool LayoutSettingsController::responseCollapsed() const {
    return hasCurrentSchema() && storedBool(settings_, kResponseCollapsedKey);
}

QString LayoutSettingsController::responseOrientation() const {
    if (!hasCurrentSchema()) {
        return QStringLiteral("auto");
    }
    return normalizedOrientation(
        settings_.value(QString::fromUtf8(kResponseOrientationKey)).toString());
}

void LayoutSettingsController::setExplorerCollapsed(const bool collapsed) {
    if (explorerCollapsed() == collapsed) {
        return;
    }
    ensureCurrentSchema();
    settings_.setValue(QString::fromUtf8(kExplorerCollapsedKey), collapsed);
    emit explorerCollapsedChanged();
}

void LayoutSettingsController::setResponseCollapsed(const bool collapsed) {
    if (responseCollapsed() == collapsed) {
        return;
    }
    ensureCurrentSchema();
    settings_.setValue(QString::fromUtf8(kResponseCollapsedKey), collapsed);
    emit responseCollapsedChanged();
}

void LayoutSettingsController::setResponseOrientation(const QString& orientation) {
    const QString normalized{normalizedOrientation(orientation)};
    if (responseOrientation() == normalized) {
        return;
    }
    ensureCurrentSchema();
    settings_.setValue(QString::fromUtf8(kResponseOrientationKey), normalized);
    emit responseOrientationChanged();
}

QVariantList LayoutSettingsController::loadSplitter(const QString& name) const {
    if (!hasCurrentSchema() || name.isEmpty()) {
        return {};
    }
    const QList<int> sizes{LayoutSettings::loadSplitter(settings_, name)};
    if (sizes.size() != 2 || sizes.at(0) <= 0 || sizes.at(1) <= 0) {
        return {};
    }
    return {sizes.at(0), sizes.at(1)};
}

bool LayoutSettingsController::saveSplitter(const QString& name,
                                            const int first,
                                            const int second) {
    if (name.isEmpty() || first <= 0 || second <= 0) {
        return false;
    }
    ensureCurrentSchema();
    LayoutSettings::saveSplitter(settings_, name, QList<int>{first, second});
    return true;
}

bool LayoutSettingsController::hasCurrentSchema() const {
    const QString key{QString::fromUtf8(kSchemaVersionKey)};
    if (!settings_.contains(key)) {
        return true;
    }
    const QVariant value{settings_.value(key)};
    return value.metaType().id() == QMetaType::Int && value.toInt() == kSchemaVersion;
}

void LayoutSettingsController::ensureCurrentSchema() {
    const QString key{QString::fromUtf8(kSchemaVersionKey)};
    if (!settings_.contains(key)) {
        settings_.setValue(key, kSchemaVersion);
        return;
    }
    if (hasCurrentSchema()) {
        return;
    }

    settings_.remove(QString::fromUtf8(kExplorerCollapsedKey));
    settings_.remove(QString::fromUtf8(kResponseCollapsedKey));
    settings_.remove(QString::fromUtf8(kResponseOrientationKey));
    settings_.remove(QString::fromUtf8(kSplitterGroup));
    settings_.setValue(key, kSchemaVersion);
}

}  // namespace reqloom::desktop::qml
