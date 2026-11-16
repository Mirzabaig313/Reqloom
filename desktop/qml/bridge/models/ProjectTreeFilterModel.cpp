// ProjectTreeFilterModel — see header.
#include "ProjectTreeFilterModel.h"

#include "ProjectTreeModel.h"

#include "widgets/FuzzyMatch.h"

#include <QtCore/QStringList>

namespace reqloom::desktop::qml {

ProjectTreeFilterModel::ProjectTreeFilterModel(QObject* parent) : QSortFilterProxyModel(parent) {
    // Keep a parent visible when any descendant matches.
    setRecursiveFilteringEnabled(true);
}

void ProjectTreeFilterModel::setFilterText(const QString& text) {
    if (text == filterText_) {
        return;
    }
    filterText_ = text;
#if QT_VERSION >= QT_VERSION_CHECK(6, 11, 0)
    beginFilterChange();
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
#else
    invalidateFilter();
#endif
    emit filterTextChanged();
}

QString ProjectTreeFilterModel::nodeKey(const QModelIndex& index) const {
    if (!index.isValid()) {
        return {};
    }
    // 0x1F (unit separator) can't appear in ids/names; field order must match ExplorerPanel.qml
    // keyForDelegate().
    const QChar sep(u'\x1f');
    return index.data(ProjectTreeModel::KindRole).toString() + sep +
           index.data(ProjectTreeModel::ProjectRootRole).toString() + sep +
           index.data(ProjectTreeModel::ResourceIdRole).toString() + sep +
           index.data(ProjectTreeModel::OperationIdRole).toString() + sep +
           index.data(ProjectTreeModel::NameRole).toString();
}

bool ProjectTreeFilterModel::filterAcceptsRow(int sourceRow,
                                              const QModelIndex& sourceParent) const {
    const QString needle = filterText_.trimmed();
    if (needle.isEmpty()) {
        return true;
    }
    const QModelIndex idx = sourceModel()->index(sourceRow, 0, sourceParent);
    if (!idx.isValid()) {
        return false;
    }
    // Only operation rows match directly; other rows survive via recursive filtering.
    if (idx.data(ProjectTreeModel::KindRole).toString() != QLatin1String("operation")) {
        return false;
    }
    const QString opId = idx.data(ProjectTreeModel::OperationIdRole).toString();
    const QString method = idx.data(ProjectTreeModel::MethodRole).toString();
    const QString path = idx.data(ProjectTreeModel::PathRole).toString();
    const QString name = idx.data(ProjectTreeModel::NameRole).toString();

    // Space-separated tokens are AND-ed; each may fuzzy-match any operation field (id, verb, path,
    // name).
    const QStringList tokens = needle.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString& token : tokens) {
        const bool hit =
            widgets::fuzzy::matches(token, opId) || widgets::fuzzy::matches(token, method) ||
            widgets::fuzzy::matches(token, path) || widgets::fuzzy::matches(token, name);
        if (!hit) {
            return false;
        }
    }
    return true;
}

}  // namespace reqloom::desktop::qml
