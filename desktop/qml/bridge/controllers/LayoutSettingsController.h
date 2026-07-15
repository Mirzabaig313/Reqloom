// QML-facing persistence for fluid shell layout state.
#pragma once

#include <QtQml/qqmlregistration.h>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QVariantList>

class QJSEngine;
class QQmlEngine;
class QSettings;

namespace reqloom::desktop::qml {

class LayoutSettingsController final : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(int schemaVersion READ schemaVersion CONSTANT)
    Q_PROPERTY(bool explorerCollapsed READ explorerCollapsed WRITE setExplorerCollapsed NOTIFY
                   explorerCollapsedChanged)
    Q_PROPERTY(bool responseCollapsed READ responseCollapsed WRITE setResponseCollapsed NOTIFY
                   responseCollapsedChanged)
    Q_PROPERTY(QString responseOrientation READ responseOrientation WRITE setResponseOrientation
                   NOTIFY responseOrientationChanged)

public:
    /// Creates a controller backed by the supplied settings store.
    explicit LayoutSettingsController(QSettings& settings, QObject* parent = nullptr);
    ~LayoutSettingsController() noexcept override = default;

    LayoutSettingsController(const LayoutSettingsController&) = delete;
    LayoutSettingsController& operator=(const LayoutSettingsController&) = delete;
    LayoutSettingsController(LayoutSettingsController&&) = delete;
    LayoutSettingsController& operator=(LayoutSettingsController&&) = delete;

    /// Returns the process-wide QML singleton.
    static LayoutSettingsController* create(QQmlEngine*, QJSEngine*);

    [[nodiscard]] int schemaVersion() const noexcept { return kSchemaVersion; }
    [[nodiscard]] bool explorerCollapsed() const;
    [[nodiscard]] bool responseCollapsed() const;
    [[nodiscard]] QString responseOrientation() const;

    void setExplorerCollapsed(bool collapsed);
    void setResponseCollapsed(bool collapsed);
    void setResponseOrientation(const QString& orientation);

    /// Loads a named two-pane splitter record, or an empty list when invalid.
    Q_INVOKABLE [[nodiscard]] QVariantList loadSplitter(const QString& name) const;

    /// Persists two positive splitter sizes for a non-empty name.
    Q_INVOKABLE [[nodiscard]] bool saveSplitter(const QString& name, int first, int second);

signals:
    void explorerCollapsedChanged();
    void responseCollapsedChanged();
    void responseOrientationChanged();

private:
    [[nodiscard]] bool hasCurrentSchema() const;
    void ensureCurrentSchema();

    static constexpr int kSchemaVersion{1};
    QSettings& settings_;
};

}  // namespace reqloom::desktop::qml
