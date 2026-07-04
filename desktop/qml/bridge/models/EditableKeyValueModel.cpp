// EditableKeyValueModel — see header.
#include "EditableKeyValueModel.h"

#include <utility>

namespace reqloom::desktop::qml {

EditableKeyValueModel::EditableKeyValueModel(QObject* parent) : QAbstractListModel(parent) {
    rows_.emplace_back(QString{}, QString{});
}

int EditableKeyValueModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant EditableKeyValueModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(rows_.size())) {
        return {};
    }
    const auto& row = rows_[static_cast<std::size_t>(index.row())];
    switch (role) {
        case KeyRole:
            return row.first;
        case ValueRole:
            return row.second;
        case IsGhostRole:
            return index.row() == static_cast<int>(rows_.size()) - 1 &&
                   rowIsBlank(static_cast<std::size_t>(index.row()));
        default:
            return {};
    }
}

QHash<int, QByteArray> EditableKeyValueModel::roleNames() const {
    return {{KeyRole, "key"}, {ValueRole, "value"}, {IsGhostRole, "isGhost"}};
}

bool EditableKeyValueModel::rowIsBlank(std::size_t row) const {
    if (row >= rows_.size()) {
        return false;
    }
    return rows_[row].first.isEmpty() && rows_[row].second.isEmpty();
}

void EditableKeyValueModel::ensureTrailingGhost() {
    if (rows_.empty() || !rowIsBlank(rows_.size() - 1)) {
        const int at = static_cast<int>(rows_.size());
        beginInsertRows({}, at, at);
        rows_.emplace_back(QString{}, QString{});
        endInsertRows();
    }
}

void EditableKeyValueModel::setKey(int row, const QString& key) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) {
        return;
    }
    rows_[static_cast<std::size_t>(row)].first = key;
    const QModelIndex idx = index(row, 0);
    emit dataChanged(idx, idx, {KeyRole, IsGhostRole});
    ensureTrailingGhost();
}

void EditableKeyValueModel::setValue(int row, const QString& value) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) {
        return;
    }
    rows_[static_cast<std::size_t>(row)].second = value;
    const QModelIndex idx = index(row, 0);
    emit dataChanged(idx, idx, {ValueRole, IsGhostRole});
    ensureTrailingGhost();
}

void EditableKeyValueModel::removeRow(int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) {
        return;
    }
    // Never remove the trailing ghost row.
    if (row == static_cast<int>(rows_.size()) - 1 && rowIsBlank(static_cast<std::size_t>(row))) {
        return;
    }
    beginRemoveRows({}, row, row);
    rows_.erase(rows_.begin() + row);
    endRemoveRows();
    ensureTrailingGhost();
}

void EditableKeyValueModel::setPairs(std::vector<std::pair<QString, QString>> pairs) {
    beginResetModel();
    rows_ = std::move(pairs);
    rows_.emplace_back(QString{}, QString{});
    endResetModel();
}

void EditableKeyValueModel::clearRows() {
    beginResetModel();
    rows_.clear();
    rows_.emplace_back(QString{}, QString{});
    endResetModel();
}

std::vector<std::pair<QString, QString>> EditableKeyValueModel::pairs() const {
    std::vector<std::pair<QString, QString>> out;
    out.reserve(rows_.size());
    for (const auto& [key, value] : rows_) {
        if (key.isEmpty() && value.isEmpty()) {
            continue;
        }
        out.emplace_back(key, value);
    }
    return out;
}

}  // namespace reqloom::desktop::qml
