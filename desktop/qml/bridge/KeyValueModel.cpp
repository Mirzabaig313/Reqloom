// KeyValueModel — see header.
#include "KeyValueModel.h"

namespace reqloom::desktop::qml {

int KeyValueModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant KeyValueModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(rows_.size())) {
        return {};
    }
    const auto& [key, value] = rows_[static_cast<std::size_t>(index.row())];
    switch (role) {
        case KeyRole:
            return key;
        case ValueRole:
            return value;
        default:
            return {};
    }
}

QHash<int, QByteArray> KeyValueModel::roleNames() const {
    return {{KeyRole, "key"}, {ValueRole, "value"}};
}

void KeyValueModel::reload(const std::map<std::string, std::string>& pairs) {
    beginResetModel();
    rows_.clear();
    rows_.reserve(pairs.size());
    for (const auto& [key, value] : pairs) {
        rows_.emplace_back(QString::fromStdString(key), QString::fromStdString(value));
    }
    endResetModel();
}

void KeyValueModel::reloadPairs(std::vector<std::pair<QString, QString>> pairs) {
    beginResetModel();
    rows_ = std::move(pairs);
    endResetModel();
}

void KeyValueModel::reset() {
    beginResetModel();
    rows_.clear();
    endResetModel();
}

}  // namespace reqloom::desktop::qml
