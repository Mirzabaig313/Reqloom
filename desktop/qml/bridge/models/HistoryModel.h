// HistoryModel — past runs from the engine's history store, for the history
// view. A flat list model populated by AppController from
// ExecutionEngine::listRuns(); C++ owns the rows, QML renders them.
#pragma once

#include <reqloom/engine/PublicApi.h>

#include <QtQml/qqmlregistration.h>
#include <QtCore/QAbstractListModel>
#include <QtCore/QString>

#include <cstdint>
#include <vector>

namespace reqloom::desktop::qml {

class HistoryModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned and populated by AppController")

    /// Per-run durations (ms) newest-first, for a cross-run latency trend.
    Q_PROPERTY(QVariantList durations READ durations NOTIFY rowsReloaded)
    /// Row count, exposed so QML can show/hide empty states before binding a view.
    Q_PROPERTY(int count READ count NOTIFY rowsReloaded)

public:
    enum Roles : int {
        RunIdRole = Qt::UserRole + 1,  ///< qulonglong: engine RunId value
        TargetRole,                    ///< QString: fully-qualified target op
        EnvRole,                       ///< QString: environment name
        OutcomeRole,                   ///< QString: Succeeded/Failed/Cancelled/"" (in-flight)
        StatusTokenRole,               ///< QString: success/error/warning/running (→ colour)
        StartedAtRole,                 ///< QString: ISO-8601 UTC start timestamp
        DurationMsRole,                ///< qlonglong: run duration, or -1 if unknown
        ChainSizeRole,                 ///< int: number of operations in the chain
    };

    explicit HistoryModel(QObject* parent = nullptr) : QAbstractListModel(parent) {}

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    [[nodiscard]] QVariantList durations() const;

    [[nodiscard]] int count() const { return static_cast<int>(rows_.size()); }

    /// Replace all rows with `entries` (already newest-first from the engine).
    void reload(const std::vector<engine::RunHistoryEntry>& entries);
    void reset();

signals:
    void rowsReloaded();

private:
    struct Row {
        std::uint64_t runId{0};
        QString target;
        QString env;
        QString outcome;
        QString statusToken;
        QString startedAt;
        qint64 durationMs{-1};
        int chainSize{0};
    };
    std::vector<Row> rows_;
};

}  // namespace reqloom::desktop::qml
