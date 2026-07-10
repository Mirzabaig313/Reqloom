// ProjectTreeFilterModel — see header.
#include "ProjectTreeFilterModel.h"

#include "ProjectTreeModel.h"

#include "widgets/FuzzyMatch.h"

#include <QtCore/QStringList>

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

QString ProjectTreeFilterModel::nodeKey(const QModelIndex& index) const {
    if (!index.isValid()) {
        return {};
    }
    // 0x1F (unit separator) can't appear in ids/names, so it's a safe join char.
    // Field order must match ExplorerPanel.qml keyForDelegate().
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
    // Only operation rows are matched directly; groups/resources/actors survive
    // solely via recursive filtering when a descendant operation matches.
    if (idx.data(ProjectTreeModel::KindRole).toString() != QLatin1String("operation")) {
        return false;
    }
    const QString opId = idx.data(ProjectTreeModel::OperationIdRole).toString();
    const QString method = idx.data(ProjectTreeModel::MethodRole).toString();
    const QString path = idx.data(ProjectTreeModel::PathRole).toString();
    const QString name = idx.data(ProjectTreeModel::NameRole).toString();

    // Space-separated tokens are AND-ed, and each token may fuzzy-match any of
    // the operation's fields (id / verb / path / display name). So "get
    // sections" or "post /attendance" narrow the way a user expects, instead of
    // the whole phrase having to be one subsequence of the id.
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
