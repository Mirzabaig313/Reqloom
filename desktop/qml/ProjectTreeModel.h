// ProjectTreeModel — the explorer tree (QML Migration Roadmap WS-A). Mirrors
// the old ProjectExplorerWidget structure: an "Actors" group and a "Resources"
// group, the latter holding resource folders → operation leaves → saved-example
// child rows. C++ owns the tree; QML (TreeView) renders it. Populated from the
// loaded engine::Project; the filter proxy on top reuses FuzzyMatch.
#pragma once

#include <reqloom/engine/PublicApi.h>

#include <QtQml/qqmlregistration.h>
#include <QtCore/QAbstractItemModel>
#include <QtCore/QHash>
#include <QtCore/QMap>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <cstdint>
#include <memory>
#include <vector>

namespace reqloom::desktop::qml {

/// Tree of actors + resources → operations → saved examples. One column. The
/// `kind` role lets QML pick a delegate treatment per row type; ids let the
/// controller resolve the selected/activated operation or resource.
class ProjectTreeModel : public QAbstractItemModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned and populated by AppController")

public:
    enum Roles : int {
        KindRole = Qt::UserRole +
                   1,     ///< QString: actorGroup/resourceGroup/actor/resource/operation/example
        NameRole,         ///< QString: display label for the row
        OperationIdRole,  ///< QString: fully-qualified op id ("<res>.<op>")
        ResourceIdRole,   ///< QString: resource id (folder rows)
        MethodRole,       ///< QString: HTTP verb for operation rows
        ExampleNameRole,  ///< QString: saved-example name
        TooltipRole,      ///< QString: full text shown on hover (names truncate)
    };

    explicit ProjectTreeModel(QObject* parent = nullptr);
    ~ProjectTreeModel() override;

    ProjectTreeModel(const ProjectTreeModel&) = delete;
    ProjectTreeModel& operator=(const ProjectTreeModel&) = delete;
    ProjectTreeModel(ProjectTreeModel&&) = delete;
    ProjectTreeModel& operator=(ProjectTreeModel&&) = delete;

    [[nodiscard]] QModelIndex index(int row,
                                    int column,
                                    const QModelIndex& parent = {}) const override;
    [[nodiscard]] QModelIndex parent(const QModelIndex& child) const override;
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    /// Rebuild the whole tree from `project` (actors + resources + operations).
    /// Saved-example child rows (set via `setSavedExamples`) are re-applied.
    void populate(const engine::Project& project);

    /// Clear the tree (no project loaded).
    void clear();

    /// Replace the saved-example child rows under every operation. Keyed by
    /// fully-qualified operation id. Triggers a rebuild so example rows appear
    /// beneath their operation. (Fed by the WS-C examples bridge.)
    void setSavedExamples(const QMap<QString, QStringList>& examplesByOperation);

private:
    enum class Kind : std::uint8_t {
        ActorGroup,
        ResourceGroup,
        Actor,
        Resource,
        Operation,
        Example,
    };

    struct Node {
        Kind kind{Kind::ActorGroup};
        QString name;
        QString operationId;
        QString resourceId;
        QString method;
        QString exampleName;
        QString tooltip;
        int rowInParent{0};
        Node* parent{nullptr};
        std::vector<std::unique_ptr<Node>> children;
    };

    [[nodiscard]] Node* nodeFor(const QModelIndex& index) const;
    Node* addChild(Node* parent, Kind kind);
    void rebuild();

    std::unique_ptr<Node> root_;
    std::shared_ptr<const engine::Project> project_;
    QMap<QString, QStringList> examples_;
};

}  // namespace reqloom::desktop::qml
