// TabModel — see header.
#include "TabModel.h"

#include <utility>

namespace reqloom::desktop::qml {

int TabModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(tabs_.size());
}

QVariant TabModel::data(const QModelIndex& index, int role) const {
    if (!valid(index.row())) {
        return {};
    }
    const TabState& tab = tabs_.at(static_cast<std::size_t>(index.row()));
    switch (role) {
        case KindRole:
            return static_cast<int>(tab.kind);
        case IdRole:
            return tab.id;
        case TitleRole:
            return tab.title;
        case MethodRole:
            return tab.method;
        case SubtitleRole:
            return tab.subtitle;
        case DirtyRole:
            return tab.dirty;
        default:
            return {};
    }
}

QHash<int, QByteArray> TabModel::roleNames() const {
    return {
        {KindRole, "kind"},
        {IdRole, "tabId"},
        {TitleRole, "title"},
        {MethodRole, "method"},
        {SubtitleRole, "subtitle"},
        {DirtyRole, "dirty"},
    };
}

int TabModel::indexOf(TabState::Kind kind, const QString& id) const {
    if (id.isEmpty()) {
        return -1;  // blank-id drafts never match — each opens its own tab
    }
    for (std::size_t i = 0; i < tabs_.size(); ++i) {
        if (tabs_[i].kind == kind && tabs_[i].id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int TabModel::append(TabState state) {
    const int row = static_cast<int>(tabs_.size());
    beginInsertRows({}, row, row);
    tabs_.push_back(std::move(state));
    endInsertRows();
    return row;
}

void TabModel::removeAt(int index) {
    if (!valid(index)) {
        return;
    }
    beginRemoveRows({}, index, index);
    tabs_.erase(tabs_.begin() + index);
    endRemoveRows();
}

void TabModel::clearAll() {
    if (tabs_.empty()) {
        return;
    }
    beginResetModel();
    tabs_.clear();
    endResetModel();
}

void TabModel::refreshRow(int index) {
    if (!valid(index)) {
        return;
    }
    const QModelIndex idx = createIndex(index, 0);
    emit dataChanged(idx, idx);
}

}  // namespace reqloom::desktop::qml
