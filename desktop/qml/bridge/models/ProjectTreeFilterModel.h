// ProjectTreeFilterModel — the explorer's live filter over ProjectTreeModel: keeps operation rows
// whose id or verb fuzzy-matches (via FuzzyMatch) plus their ancestors.
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

    /// Stable identity string for a tree node (kind + owning project + ids + name, joined by 0x1F),
    /// used to persist expanded rows. Empty for an invalid index. Must match ExplorerPanel's
    /// keyForDelegate() byte for byte.
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
