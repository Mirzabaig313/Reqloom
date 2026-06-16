// TimelineModel — the live run timeline (QML Migration Roadmap WS-C). A flat
// list model fed by ALL RunController signals; each streamed RunEvent appends
// (or settles) a row, mirroring the old Widgets TimelinePanel's content and
// its amber-not-red colouring for non-resolved extractions (DESIGN.md §2.5).
// C++ owns the rows; QML (TimelinePanel.qml) renders them.
#pragma once

#include <QtQml/qqmlregistration.h>
#include <QtCore/QAbstractListModel>
#include <QtCore/QHash>
#include <QtCore/QString>
#include <QtCore/QVariantList>
#include <QtCore/QVariantMap>

#include <cstdint>
#include <vector>

namespace reqloom::desktop::qml {

class TimelineModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned and populated by AppController")

    /// Per-response latency bars for the sparkline: a list of
    /// `{ ms: real, token: string, op: string }` in request order.
    Q_PROPERTY(QVariantList latencyBars READ latencyBars NOTIFY latenciesChanged)
    /// Summary + Freedman–Diaconis histogram of the run's latencies:
    /// `{ count, min, max, mean, median, p95, binWidth, start, bins:[int] }`.
    Q_PROPERTY(QVariantMap latencyStats READ latencyStats NOTIFY latenciesChanged)

public:
    /// Row kind, mirroring the RunEvent types the old TimelinePanel rendered.
    enum class Kind : std::uint8_t {
        RunStart,
        Step,
        Request,
        Response,
        Extraction,
        Assertion,
        Skip,
        Fail,
        RunEnd,
    };

    enum Roles : int {
        KindRole = Qt::UserRole + 1,  ///< QString: runStart/step/request/response/…
        IndexRole,                    ///< int: 1-based step number (0 for run-level rows)
        TitleRole,                    ///< QString: leading label (step name, "→ request", …)
        DetailRole,                   ///< QString: trailing descriptive text
        StatusTokenRole,  ///< QString: status vocabulary token (→ DesignTokens colour)
        StatusLabelRole,  ///< QString: short badge label ("running", "HTTP 200", …)
        ValueRole,        ///< QString: tooltip payload (masked headers / full value)
    };

    explicit TimelineModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    [[nodiscard]] QVariantList latencyBars() const { return latencyBars_; }
    [[nodiscard]] QVariantMap latencyStats() const;

signals:
    void latenciesChanged();

public slots:
    // One slot per RunController signal. Signatures match the signals so the
    // controller connects them directly (function-pointer form, receiver-bound).
    void onRunStarted(QString target, int chainSize, QString environment);
    void onStepStarted(int index, QString op, int attempt);
    void onStepSkipped(int index, QString op, QString reason);
    void onRequestPrepared(
        int index, QString method, QString url, QString maskedHeaders, int bodySize);
    void onResponseReceived(
        int index, int status, QString headers, int bodySize, qint64 elapsedMs, QString body);
    void onExtractionCompleted(int index,
                               QString op,
                               QString variableName,
                               QString sourcePath,
                               QString outcome,
                               QString value);
    void onAssertionCompleted(int index, QString op, QString name, QString expr, bool passed);
    void onStepFailed(int index, QString op, QString code, QString detail);
    void onRunEnded(QString outcome);

    /// Clear all rows (a fresh run is starting, or a project loaded).
    void reset();

private:
    struct Row {
        Kind kind{Kind::Step};
        int stepIndex{0};
        QString title;
        QString detail;
        QString statusToken;
        QString statusLabel;
        QString value;
    };

    /// Find the existing top-level step row for `index`, or append one. Mirrors
    /// the old TimelinePanel::stepRow so streamed events settle the same row.
    [[nodiscard]] int stepRowFor(int index, const QString& op);
    void appendRow(Row row);
    /// QModelIndex for row position `at`. A named helper avoids the `index()`
    /// member being shadowed by the `int index` slot parameters.
    [[nodiscard]] QModelIndex rowIndex(int at) const { return index(at, 0); }

    std::vector<Row> rows_;
    // Per-response latency samples (ms) for the run, in request order, plus the
    // QML-facing bar list ({ms, token, op}); kept in lockstep.
    std::vector<double> latencyMs_;
    QVariantList latencyBars_;
    // stepIndex → position in rows_. Positions are stable: rows are only ever
    // appended (never removed) within a run, so settling a step is O(1).
    QHash<int, int> stepRowByIndex_;
    // Position of the run header row, so onRunEnded can settle its stale
    // "running" badge to the final outcome instead of leaving it spinning.
    int runStartRow_{-1};
};

}  // namespace reqloom::desktop::qml
