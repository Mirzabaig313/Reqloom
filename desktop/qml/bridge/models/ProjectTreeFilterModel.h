// ProjectTreeFilterModel — the explorer's live filter
// Wraps ProjectTreeModel and keeps an operation row when the query
// fuzzy-matches its id OR its method verb, plus every ancestor of a match
// (recursive filtering). Reuses the pure FuzzyMatch matcher so the explorer
// and the command palette rank consistently. Empty query shows everything.
#pragma once

#include <QtCore/QSortFilterProxyModel>
#include <QtCore/QString>

namespace reqloom::desktop::qml {

class ProjectTreeFilterModel : public QSortFilterProxyModel {
    Q_OBJECT

    Q_PROPERTY(QString filterText READ filterText WRITE setFilterText NOTIFY filterTextChanged)

public:
    explicit ProjectTreeFilterModel(QObject* parent = nullptr);

    [[nodiscard]] QString filterText() const { return filterText_; }
    void setFilterText(const QString& text);

    /// Stable identity string for a tree node (proxy index), used by the
    /// explorer to persist which rows are expanded across model rebuilds and
    /// app restarts. Composed of the node's kind + owning project + ids + name,
    /// joined by the unit-separator (0x1F). Empty for an invalid index.
    /// Must stay byte-for-byte in sync with ExplorerPanel's keyForDelegate().
    [[nodiscard]] Q_INVOKABLE QString nodeKey(const QModelIndex& index) const;

signals:
    void filterTextChanged();

protected:
    [[nodiscard]] bool filterAcceptsRow(int sourceRow,
                                        const QModelIndex& sourceParent) const override;

private:
    QString filterText_;
};

}  // namespace reqloom::desktop::qml
