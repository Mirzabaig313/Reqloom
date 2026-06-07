// DependencyEditModel — see header.
#include "DependencyEditModel.h"

#include <set>

namespace reqloom::desktop::qml {

DependencyEditModel::DependencyEditModel(QObject* parent) : QAbstractListModel(parent) {
    rows_.emplace_back(QString{});
}

int DependencyEditModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant DependencyEditModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(rows_.size())) {
        return {};
    }
    const QString& value = rows_[static_cast<std::size_t>(index.row())];
    switch (role) {
        case ValueRole:
            return value;
        case IsGhostRole:
            return value.isEmpty();
        default:
            return {};
    }
}

QHash<int, QByteArray> DependencyEditModel::roleNames() const {
    return {{ValueRole, "value"}, {IsGhostRole, "isGhost"}};
}

void DependencyEditModel::setCandidates(const QStringList& operationIds) {
    candidates_ = operationIds;
    beginResetModel();
    rows_.clear();
    rows_.emplace_back(QString{});
    endResetModel();
    emit candidatesChanged();
}

void DependencyEditModel::ensureTrailingGhost() {
    if (rows_.empty() || !rows_.back().isEmpty()) {
        const int at = static_cast<int>(rows_.size());
        beginInsertRows({}, at, at);
        rows_.emplace_back(QString{});
        endInsertRows();
    }
}

void DependencyEditModel::setSelection(int row, const QString& value) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) {
        return;
    }
    rows_[static_cast<std::size_t>(row)] = value;
    const QModelIndex idx = index(row, 0);
    emit dataChanged(idx, idx, {ValueRole, IsGhostRole});
    ensureTrailingGhost();
}

void DependencyEditModel::removeRow(int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) {
        return;
    }
    if (row == static_cast<int>(rows_.size()) - 1 && rows_.back().isEmpty()) {
        return;
    }
    beginRemoveRows({}, row, row);
    rows_.erase(rows_.begin() + row);
    endRemoveRows();
    ensureTrailingGhost();
}

std::vector<std::string> DependencyEditModel::dependencies() const {
    std::vector<std::string> out;
    std::set<QString> seen;
    for (const QString& value : rows_) {
        if (value.isEmpty()) {
            continue;
        }
        if (seen.insert(value).second) {
            out.push_back(value.toStdString());
        }
    }
    return out;
}

}  // namespace reqloom::desktop::qml
