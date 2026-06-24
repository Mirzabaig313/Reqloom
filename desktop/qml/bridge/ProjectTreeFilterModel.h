// ProjectTreeFilterModel — the explorer's live filter (QML Migration Roadmap
// WS-A). Wraps ProjectTreeModel and keeps an operation row when the query
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

signals:
    void filterTextChanged();

protected:
    [[nodiscard]] bool filterAcceptsRow(int sourceRow,
                                        const QModelIndex& sourceParent) const override;

private:
    QString filterText_;
};

}  // namespace reqloom::desktop::qml
