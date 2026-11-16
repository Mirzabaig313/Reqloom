// HistoryModel — see header. Maps RunHistoryEntry rows to QML roles.
#include "HistoryModel.h"

#include <QtCore/QVariantList>

namespace reqloom::desktop::qml {

namespace {

// Run outcome → badge status token; an empty outcome means RunEnded was never recorded, shown as
// "running".
[[nodiscard]] QString tokenForOutcome(const QString& outcome) {
    if (outcome == QStringLiteral("Succeeded")) {
        return QStringLiteral("success");
    }
    if (outcome == QStringLiteral("Failed")) {
        return QStringLiteral("error");
    }
    if (outcome == QStringLiteral("Cancelled")) {
        return QStringLiteral("warning");
    }
    return QStringLiteral("running");
}

}  // namespace

int HistoryModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(rows_.size());
}

QVariant HistoryModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(rows_.size())) {
        return {};
    }
    const Row& row = rows_[static_cast<std::size_t>(index.row())];
    switch (role) {
        case RunIdRole:
            return QVariant::fromValue<qulonglong>(row.runId);
        case TargetRole:
            return row.target;
        case EnvRole:
            return row.env;
        case OutcomeRole:
            return row.outcome;
        case StatusTokenRole:
            return row.statusToken;
        case StartedAtRole:
            return row.startedAt;
        case DurationMsRole:
            return QVariant::fromValue<qint64>(row.durationMs);
        case ChainSizeRole:
            return row.chainSize;
        default:
            return {};
    }
}

QHash<int, QByteArray> HistoryModel::roleNames() const {
    return {
        {RunIdRole, "runId"},
        {TargetRole, "target"},
        {EnvRole, "env"},
        {OutcomeRole, "outcome"},
        {StatusTokenRole, "statusToken"},
        {StartedAtRole, "startedAt"},
        {DurationMsRole, "durationMs"},
        {ChainSizeRole, "chainSize"},
    };
}

QVariantList HistoryModel::durations() const {
    QVariantList out;
    out.reserve(static_cast<qsizetype>(rows_.size()));
    for (const auto& row : rows_) {
        out.append(QVariant::fromValue<qint64>(row.durationMs));
    }
    return out;
}

void HistoryModel::reload(const std::vector<engine::RunHistoryEntry>& entries) {
    beginResetModel();
    rows_.clear();
    rows_.reserve(entries.size());
    for (const auto& e : entries) {
        Row row;
        row.runId = e.runId.value;
        row.target = QString::fromStdString(e.target.value);
        row.env = QString::fromStdString(e.envName);
        row.outcome = QString::fromStdString(e.outcome);
        row.statusToken = tokenForOutcome(row.outcome);
        row.startedAt = QString::fromStdString(e.startedAt);
        row.durationMs = e.elapsedMs;
        row.chainSize = static_cast<int>(e.chainSize);
        rows_.push_back(std::move(row));
    }
    endResetModel();
    emit rowsReloaded();
}

void HistoryModel::reset() {
    beginResetModel();
    rows_.clear();
    endResetModel();
    emit rowsReloaded();
}

}  // namespace reqloom::desktop::qml
