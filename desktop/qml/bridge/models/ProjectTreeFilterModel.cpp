// ProjectTreeFilterModel — see header.
#include "ProjectTreeFilterModel.h"

#include "ProjectTreeModel.h"

#include "widgets/FuzzyMatch.h"

namespace reqloom::desktop::qml {

ProjectTreeFilterModel::ProjectTreeFilterModel(QObject* parent) : QSortFilterProxyModel(parent) {
    // Keep a parent visible when any descendant matches — mirrors the old
    // explorer (a resource folder stays if one of its operations matches).
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
    // Only operation rows are matched directly; groups/resources/actors survive
    // solely via recursive filtering when a descendant operation matches.
    if (idx.data(ProjectTreeModel::KindRole).toString() != QLatin1String("operation")) {
        return false;
    }
    const QString opId = idx.data(ProjectTreeModel::OperationIdRole).toString();
    const QString method = idx.data(ProjectTreeModel::MethodRole).toString();
    return widgets::fuzzy::matches(needle, opId) || widgets::fuzzy::matches(needle, method);
}

}  // namespace reqloom::desktop::qml
