// TimelineModel — see header. Mirrors the old TimelinePanel.cpp content.
#include "TimelineModel.h"

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
    endResetModel();
}

void TimelineModel::onRunStarted(QString target, int chainSize, QString environment) {
    reset();
    Row row;
    row.kind = Kind::RunStart;
    row.title = QStringLiteral("Running %1").arg(target);
    row.detail = QStringLiteral("%1 steps  ·  env=%2").arg(chainSize).arg(environment);
    row.statusToken = QStringLiteral("running");
    row.statusLabel = QStringLiteral("running");
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
    row.detail = QStringLiteral("%1 %2  (%3 body bytes)").arg(method, url).arg(bodySize);
    row.statusToken = QStringLiteral("neutral");
    row.value = maskedHeaders;
    appendRow(std::move(row));
}

void TimelineModel::onResponseReceived(
    int index, int status, QString headers, int bodySize, qint64 elapsedMs, QString /*body*/) {
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
    row.value = headers;
    appendRow(std::move(row));
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
    row.detail = QStringLiteral("[%1] %2").arg(code, detail);
    const QModelIndex idx = rowIndex(at);
    emit dataChanged(idx, idx);
}

void TimelineModel::onRunEnded(QString outcome) {
    Row row;
    row.kind = Kind::RunEnd;
    row.title = outcome;
    row.detail = outcome;
    if (outcome.contains(QLatin1String("Succeeded"))) {
        row.statusToken = QStringLiteral("success");
    } else if (outcome.contains(QLatin1String("Cancelled"))) {
        row.statusToken = QStringLiteral("cancelled");
    } else if (outcome.contains(QLatin1String("Failed"))) {
        row.statusToken = QStringLiteral("error");
    } else {
        row.statusToken = QStringLiteral("neutral");
    }
    row.statusLabel = outcome;
    appendRow(std::move(row));
}

}  // namespace reqloom::desktop::qml
