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
        KindRole =
            Qt::UserRole +
            1,     ///< QString: project/actorGroup/resourceGroup/actor/resource/operation/example
        NameRole,  ///< QString: display label for the row
        OperationIdRole,  ///< QString: fully-qualified op id ("<res>.<op>")
        ResourceIdRole,   ///< QString: resource id (folder rows)
        MethodRole,       ///< QString: HTTP verb for operation rows
        ExampleNameRole,  ///< QString: saved-example name
        TooltipRole,      ///< QString: full text shown on hover (names truncate)
        CountRole,        ///< int: child count for folder rows (0 for leaves)
        StatusRole,       ///< int: HTTP status of a saved-example row (0 otherwise)
        StatusTokenRole,  ///< QString: success/warning/error token for the status badge
        ProjectRootRole,  ///< QString: owning project's root path (every row carries it)
        ActiveRole,       ///< bool: true on the active project's row
    };

    /// One saved-example child row: its display name + the HTTP status it
    /// captured (for the status badge in the explorer).
    struct ExampleRow {
        QString name;
        int status{0};
    };

    /// One open collection fed to the aggregated tree: its root path, display
    /// name, the loaded project, and whether it's the active one.
    struct ProjectEntry {
        QString root;
        QString name;
        std::shared_ptr<const engine::Project> project;
        bool active{false};
    };

    /// Composite key for `setSavedExamples`, scoping example rows to a specific
    /// project so identically-named operations in different collections don't
    /// share example rows.
    [[nodiscard]] static QString exampleKey(const QString& projectRoot, const QString& operationId);

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

    /// Rebuild the whole tree from every open collection: one Project node per
    /// entry, each holding its Actors + Resources subtrees. Saved-example child
    /// rows (set via `setSavedExamples`) are re-applied. An empty list clears
    /// the tree.
    void populate(const std::vector<ProjectEntry>& projects);

    /// Clear the tree (no project loaded).
    void clear();

    /// Replace the saved-example child rows under every operation. Keyed by
    /// fully-qualified operation id. Triggers a rebuild so example rows appear
    /// beneath their operation. (Fed by the WS-C examples bridge.)
    void setSavedExamples(const QMap<QString, QList<ExampleRow>>& examplesByOperation);

private:
    enum class Kind : std::uint8_t {
        Project,
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
        QString projectRoot;  ///< owning project's root (set on every descendant)
        bool active{false};   ///< true on the active project node
        int exampleStatus{0};
        int rowInParent{0};
        Node* parent{nullptr};
        std::vector<std::unique_ptr<Node>> children;
    };

    [[nodiscard]] Node* nodeFor(const QModelIndex& index) const;
    Node* addChild(Node* parent, Kind kind);
    void rebuild();

    std::unique_ptr<Node> root_;
    std::vector<ProjectEntry> projects_;
    QMap<QString, QList<ExampleRow>> examples_;
};

}  // namespace reqloom::desktop::qml
