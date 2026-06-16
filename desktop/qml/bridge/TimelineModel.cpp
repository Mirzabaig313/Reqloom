// TimelineModel — see header. Mirrors the old TimelinePanel.cpp content.
#include "TimelineModel.h"

#include "../../src/widgets/LatencyStats.h"

#include <reqloom/engine/ErrorCodes.h>

namespace reqloom::desktop::qml {

namespace {

[[nodiscard]] QString kindToken(TimelineModel::Kind kind) {
    switch (kind) {
        case TimelineModel::Kind::RunStart:
            return QStringLiteral("runStart");
        case TimelineModel::Kind::Step:
            return QStringLiteral("step");
        case TimelineModel::Kind::Request:
            return QStringLiteral("request");
        case TimelineModel::Kind::Response:
            return QStringLiteral("response");
        case TimelineModel::Kind::Extraction:
            return QStringLiteral("extraction");
        case TimelineModel::Kind::Skip:
            return QStringLiteral("skip");
        case TimelineModel::Kind::Fail:
            return QStringLiteral("fail");
        case TimelineModel::Kind::RunEnd:
            return QStringLiteral("runEnd");
    }
    return QStringLiteral("step");
}

/// Turn a raw engine error code (E_SESSION_REFRESH_FAILED) into a phrase a
/// human can act on. Known codes resolve via the engine's canonical
/// `humanize` (one source of truth shared with the CLI); anything else falls
/// back to title-casing the token (strip E_, underscores → spaces).
[[nodiscard]] QString humanizeError(const QString& code) {
    if (const auto known = engine::fromCodeString(code.toStdString())) {
        const auto phrase = engine::humanize(*known);
        return QString::fromUtf8(phrase.data(), static_cast<qsizetype>(phrase.size()));
    }
    QString token = code;
    if (token.startsWith(QLatin1String("E_"))) {
        token.remove(0, 2);
    }
    token.replace(QLatin1Char('_'), QLatin1Char(' '));
    token = token.toLower();
    if (!token.isEmpty()) {
        token[0] = token[0].toUpper();
    }
    return token.isEmpty() ? QStringLiteral("Failed") : token;
}

}  // namespace

TimelineModel::TimelineModel(QObject* parent) : QAbstractListModel(parent) {}

int TimelineModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant TimelineModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(rows_.size())) {
        return {};
    }
    const Row& row = rows_[static_cast<std::size_t>(index.row())];
    switch (role) {
        case KindRole:
            return kindToken(row.kind);
        case IndexRole:
            return row.stepIndex;
        case TitleRole:
            return row.title;
        case DetailRole:
            return row.detail;
        case StatusTokenRole:
            return row.statusToken;
        case StatusLabelRole:
            return row.statusLabel;
        case ValueRole:
            return row.value;
        default:
            return {};
    }
}

QHash<int, QByteArray> TimelineModel::roleNames() const {
    return {
        {KindRole, "kind"},
        {IndexRole, "stepIndex"},
        {TitleRole, "title"},
        {DetailRole, "detail"},
        {StatusTokenRole, "statusToken"},
        {StatusLabelRole, "statusLabel"},
        {ValueRole, "value"},
    };
}

void TimelineModel::appendRow(Row row) {
    const int at = static_cast<int>(rows_.size());
    beginInsertRows({}, at, at);
    rows_.push_back(std::move(row));
    endInsertRows();
}

int TimelineModel::stepRowFor(int index, const QString& op) {
    const auto it = stepRowByIndex_.constFind(index);
    if (it != stepRowByIndex_.constEnd()) {
        return it.value();
    }
    Row row;
    row.kind = Kind::Step;
    row.stepIndex = index + 1;
    row.title = QStringLiteral("%1. %2").arg(index + 1).arg(op);
    const int at = static_cast<int>(rows_.size());
    beginInsertRows({}, at, at);
    rows_.push_back(std::move(row));
    endInsertRows();
    stepRowByIndex_.insert(index, at);
    return at;
}

void TimelineModel::reset() {
    beginResetModel();
    rows_.clear();
    stepRowByIndex_.clear();
    runStartRow_ = -1;
    endResetModel();
    if (!latencyMs_.empty() || !latencyBars_.isEmpty()) {
        latencyMs_.clear();
        latencyBars_.clear();
        emit latenciesChanged();
    }
}

QVariantMap TimelineModel::latencyStats() const {
    QVariantMap out;
    const stats::Summary s = stats::summarize(latencyMs_);
    out.insert(QStringLiteral("count"), static_cast<int>(s.count));
    out.insert(QStringLiteral("min"), s.min);
    out.insert(QStringLiteral("max"), s.max);
    out.insert(QStringLiteral("mean"), s.mean);
    out.insert(QStringLiteral("median"), s.median);
    out.insert(QStringLiteral("p95"), s.p95);

    const stats::Histogram h = stats::histogram(latencyMs_);
    out.insert(QStringLiteral("start"), h.start);
    out.insert(QStringLiteral("binWidth"), h.binWidth);
    QVariantList bins;
    bins.reserve(static_cast<qsizetype>(h.counts.size()));
    for (const int c : h.counts) {
        bins.append(c);
    }
    out.insert(QStringLiteral("bins"), bins);
    return out;
}

void TimelineModel::onRunStarted(QString target, int chainSize, QString environment) {
    reset();
    Row row;
    row.kind = Kind::RunStart;
    row.title = target;
    row.detail = QStringLiteral("%1 steps  ·  env=%2").arg(chainSize).arg(environment);
    row.statusToken = QStringLiteral("running");
    row.statusLabel = QStringLiteral("running");
    runStartRow_ = static_cast<int>(rows_.size());
    appendRow(std::move(row));
}

void TimelineModel::onStepStarted(int index, QString op, int attempt) {
    const int at = stepRowFor(index, op);
    Row& row = rows_[static_cast<std::size_t>(at)];
    row.statusToken = QStringLiteral("running");
    row.statusLabel = QStringLiteral("running");
    if (attempt > 1) {
        row.detail = QStringLiteral("attempt %1").arg(attempt);
    }
    const QModelIndex idx = rowIndex(at);
    emit dataChanged(idx, idx);
}

void TimelineModel::onStepSkipped(int index, QString op, QString reason) {
    const int at = stepRowFor(index, op);
    Row& row = rows_[static_cast<std::size_t>(at)];
    row.statusToken = QStringLiteral("skipped");
    row.statusLabel = QStringLiteral("skipped");
    row.detail = reason;
    const QModelIndex idx = rowIndex(at);
    emit dataChanged(idx, idx);
}

void TimelineModel::onRequestPrepared(
    int index, QString method, QString url, QString maskedHeaders, int bodySize) {
    Row row;
    row.kind = Kind::Request;
    row.stepIndex = index + 1;
    row.title = QStringLiteral("\u2192 request");  // → request
    // Keep the summary short so it never elides; the fully-resolved URL lives
    // in the expansion below (open the row to read / copy it).
    row.detail = QStringLiteral("%1  ·  %2 body bytes").arg(method).arg(bodySize);
    row.statusToken = QStringLiteral("neutral");
    // Expansion: the fully-resolved URL (base URL + path) up top, then the
    // masked request headers. Lets the user confirm exactly where the call
    // went without squinting at the elided one-liner.
    QString expanded = QStringLiteral("URL\n%1").arg(url);
    if (!maskedHeaders.isEmpty()) {
        expanded += QStringLiteral("\n\nHeaders\n%1").arg(maskedHeaders);
    }
    row.value = expanded;
    appendRow(std::move(row));
}

void TimelineModel::onResponseReceived(
    int index, int status, QString headers, int bodySize, qint64 elapsedMs, QString body) {
    // Settle the parent step row to a terminal status by HTTP class (§2.5).
    const auto it = stepRowByIndex_.constFind(index);
    if (it != stepRowByIndex_.constEnd()) {
        Row& step = rows_[static_cast<std::size_t>(it.value())];
        step.statusToken = status >= 500   ? QStringLiteral("error")
                           : status >= 300 ? QStringLiteral("warning")
                                           : QStringLiteral("success");
        step.statusLabel = QStringLiteral("HTTP %1").arg(status);
        const QModelIndex idx = rowIndex(it.value());
        emit dataChanged(idx, idx);
    }

    Row row;
    row.kind = Kind::Response;
    row.stepIndex = index + 1;
    row.title = QStringLiteral("\u2190 response");  // ← response
    row.detail =
        QStringLiteral("HTTP %1  ·  %2 bytes  ·  %3 ms").arg(status).arg(bodySize).arg(elapsedMs);
    // Colour the response row by HTTP class so a non-2xx stands out.
    row.statusToken = status >= 500   ? QStringLiteral("error")
                      : status >= 300 ? QStringLiteral("warning")
                                      : QStringLiteral("success");
    // Expansion: status line, headers, and the body when the run captured it
    // (off by default — see RunOptions::captureResponseBodies).
    QString expanded = QStringLiteral("Status\nHTTP %1").arg(status);
    if (!headers.isEmpty()) {
        expanded += QStringLiteral("\n\nHeaders\n%1").arg(headers);
    }
    if (!body.isEmpty()) {
        expanded += QStringLiteral("\n\nBody\n%1").arg(body);
    }
    row.value = expanded;
    appendRow(std::move(row));

    // Feed the latency sparkline: one bar per response, in request order.
    const QString token = status >= 500   ? QStringLiteral("error")
                          : status >= 300 ? QStringLiteral("warning")
                                          : QStringLiteral("success");
    latencyMs_.push_back(static_cast<double>(elapsedMs));
    QVariantMap bar;
    bar.insert(QStringLiteral("ms"), static_cast<double>(elapsedMs));
    bar.insert(QStringLiteral("token"), token);
    bar.insert(QStringLiteral("op"), QStringLiteral("step %1").arg(index + 1));
    latencyBars_.append(bar);
    emit latenciesChanged();
}

void TimelineModel::onExtractionCompleted(int index,
                                          QString /*op*/,
                                          QString variableName,
                                          QString sourcePath,
                                          QString outcome,
                                          QString value) {
    Row row;
    row.kind = Kind::Extraction;
    row.stepIndex = index + 1;
    row.title = variableName;
    const bool resolved = (outcome == QLatin1String("resolved"));
    if (resolved) {
        // Resolved extractions read as success (green).
        row.statusToken = QStringLiteral("success");
        row.statusLabel = QStringLiteral("=");
        row.detail = value;
        row.value = value;
    } else {
        // null / missing / invalid is a non-error condition that still demands
        // attention — DESIGN.md §2.5 reserves status.warning (AMBER) for it,
        // never red. Mirrors the old TimelinePanel exactly.
        row.statusToken = QStringLiteral("warning");
        row.statusLabel = outcome;
        row.detail = QStringLiteral("%1  (%2)").arg(outcome, sourcePath);
        row.value = sourcePath;
    }
    appendRow(std::move(row));
}

void TimelineModel::onStepFailed(int index, QString op, QString code, QString detail) {
    const int at = stepRowFor(index, op);
    Row& row = rows_[static_cast<std::size_t>(at)];
    row.statusToken = QStringLiteral("error");
    row.statusLabel = QStringLiteral("failed");
    const QString friendly = humanizeError(code);
    row.detail = detail.isEmpty() ? friendly : QStringLiteral("%1 — %2").arg(friendly, detail);
    // Expansion: the full failure reason (often a network/auth message that is
    // too long for the row) plus the raw code for support / bug reports.
    QString expanded;
    if (!detail.isEmpty()) {
        expanded = detail + QStringLiteral("\n\n");
    }
    expanded += QStringLiteral("Error code: %1").arg(code);
    row.value = expanded;
    const QModelIndex idx = rowIndex(at);
    emit dataChanged(idx, idx);
}

void TimelineModel::onRunEnded(QString outcome) {
    // Classify the outcome once; reused for the header badge and summary row.
    QString token;
    QString summary;
    QString badge;
    if (outcome.contains(QLatin1String("Succeeded"))) {
        token = QStringLiteral("success");
        summary = QStringLiteral("Run finished");
        badge = QStringLiteral("done");
    } else if (outcome.contains(QLatin1String("Cancelled"))) {
        token = QStringLiteral("cancelled");
        summary = QStringLiteral("Run cancelled");
        badge = QStringLiteral("cancelled");
    } else if (outcome.contains(QLatin1String("Failed"))) {
        token = QStringLiteral("error");
        summary = QStringLiteral("Run failed");
        badge = QStringLiteral("failed");
    } else {
        token = QStringLiteral("neutral");
        summary = outcome;
        badge = outcome;
    }

    // Settle the run header so its "running" badge reflects the final state
    // instead of spinning forever after the run ends.
    if (runStartRow_ >= 0 && runStartRow_ < static_cast<int>(rows_.size())) {
        Row& header = rows_[static_cast<std::size_t>(runStartRow_)];
        header.statusToken = token;
        header.statusLabel = badge;
        const QModelIndex idx = rowIndex(runStartRow_);
        emit dataChanged(idx, idx);
    }

    // Summary row: one clean phrase, coloured by token. No redundant badge or
    // detail (the header already carries the status badge).
    Row row;
    row.kind = Kind::RunEnd;
    row.title = summary;
    row.statusToken = token;
    appendRow(std::move(row));
}

}  // namespace reqloom::desktop::qml
