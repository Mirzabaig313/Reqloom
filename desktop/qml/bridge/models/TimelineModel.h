// TimelineModel — the live run timeline . A flat
// list model fed by ALL RunController signals; each streamed RunEvent appends
// (or settles) a row, mirroring the old Widgets TimelinePanel's content and
// its amber-not-red colouring for non-resolved extractions (DESIGN..
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

    /// The step the inspector is pinned to, as a 1-based step number; `0` means
    /// "follow the live run" and is the resting state. Owned here rather than in
    /// QML so it rides along in `Snapshot` and is therefore per-tab for free.
    ///
    /// Writing a step that does not exist clears the selection instead of
    /// pinning a dead step. QML must treat this as write-on-gesture /
    /// read-for-render: never bind a view's `currentIndex` to it *and* assign it
    /// back from that view's change handler, or the two form a loop.
    Q_PROPERTY(int selectedStep READ selectedStep WRITE setSelectedStep NOTIFY selectionChanged)
    /// Operation id owning `selectedStep`, or empty when nothing is selected.
    /// Lets the chain graph and the explorer highlight the same operation the
    /// timeline is showing without either of them knowing a timeline exists.
    Q_PROPERTY(QString selectedOperationId READ selectedOperationId NOTIFY selectionChanged)

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
        StatusTokenRole,   ///< QString: status vocabulary token (→ DesignTokens colour)
        StatusLabelRole,   ///< QString: short badge label ("running", "HTTP 200", …)
        ValueRole,         ///< QString: tooltip payload (masked headers / full value)
        MethodRole,        ///< QString: HTTP method for request rows ("GET"/"POST"); else empty
        PathRole,          ///< QString: request URL path for request rows; else empty
        SizeRole,          ///< QString: pre-formatted payload size ("66 B", "3.9 KB")
        ClockRole,         ///< QString: wall-clock time the row was recorded ("10:24:10")
        DurationRole,      ///< QString: pre-formatted duration (response rows + step totals)
        SubLabelRole,      ///< QString: sub-step badge for child rows ("1.1", "1.2")
        OpRole,            ///< QString: owning operation id on step + extraction rows
        VariableNameRole,  ///< QString: bare extracted variable on extraction rows; else empty
    };

private:
    struct Row {
        Kind kind{Kind::Step};
        int stepIndex{0};
        QString title;
        QString detail;
        QString statusToken;
        QString statusLabel;
        QString value;
        QString method;        ///< Request rows only.
        QString path;          ///< Request rows only (URL path).
        QString sizeText;      ///< Request + response rows.
        QString clockText;     ///< Request + response rows.
        QString durationText;  ///< Response rows + step-header totals.
        QString subLabel;      ///< Child rows: "1.1", "1.2", …
        QString op;            ///< Step + extraction rows: the owning operation id.
        QString variableName;  ///< Extraction rows: the bare extracted variable name.
    };

public:
    /// A full copy of the timeline's state, so a multi-tab host can park one
    /// tab's run timeline and restore it on switch instead of losing it. Holds
    /// the (private) Row rows plus the latency/step bookkeeping. Opaque to
    /// callers — they only take/restore it, never inspect it.
    struct Snapshot {
        std::vector<Row> rows;
        std::vector<double> latencyMs;
        QVariantList latencyBars;
        QHash<int, int> stepRowByIndex;
        QHash<int, int> stepChildSeq;
        QHash<int, double> stepMs;
        double runTotalMs{0.0};
        int runChainSize{0};
        QString runEnv;
        int runStartRow{-1};
        int selectedStep{0};
    };

    explicit TimelineModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    [[nodiscard]] QVariantList latencyBars() const { return latencyBars_; }
    [[nodiscard]] QVariantMap latencyStats() const;

    /// Model row of the step header for a 1-based step number, or -1 if absent.
    /// Lets the sparkline scroll the timeline to a clicked bar's step.
    [[nodiscard]] Q_INVOKABLE int rowForStep(int stepNumber) const;

    /// 1-based step number that ran `operationId`, or 0 when this run has no such
    /// step. Lets a consumer named on one row be jumped to on another.
    [[nodiscard]] Q_INVOKABLE int stepForOperation(const QString& operationId) const;

    [[nodiscard]] int selectedStep() const { return selectedStep_; }
    /// Pin the inspector to a 1-based step. `0`, or any step with no row,
    /// clears the selection. No-ops (and emits nothing) when unchanged.
    void setSelectedStep(int stepNumber);
    [[nodiscard]] QString selectedOperationId() const;

    /// Copy out the full timeline state (for a tab about to be backgrounded).
    [[nodiscard]] Snapshot takeSnapshot() const;
    /// Replace the timeline with a previously taken snapshot (tab activated).
    void restoreSnapshot(Snapshot snapshot);

signals:
    void latenciesChanged();
    /// Emitted when `selectedStep` changes, and after a snapshot restore (the
    /// step number can survive while the operation under it differs).
    void selectionChanged();

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
    // Per-step running child counter (request/response) for "N.M" sub-badges,
    // and per-step accumulated response time for the step-header total.
    QHash<int, int> stepChildSeq_;
    QHash<int, double> stepMs_;
    // Whole-run totals + the run header's reconstructable parts.
    double runTotalMs_{0.0};
    int runChainSize_{0};
    QString runEnv_;
    // Position of the run header row, so onRunEnded can settle its stale
    // "running" badge to the final outcome instead of leaving it spinning.
    int runStartRow_{-1};
    // 1-based pinned step; 0 = follow the live run.
    int selectedStep_{0};
};

}  // namespace reqloom::desktop::qml
