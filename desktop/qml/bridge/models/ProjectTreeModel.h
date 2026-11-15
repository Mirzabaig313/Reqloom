// ProjectTreeModel — the explorer tree: Actors and Resources groups, resources → operations →
// saved-example rows. C++ owns it; QML (TreeView) renders it.
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

/// Tree of actors + resources → operations → saved examples. The `kind` role selects the delegate;
/// ids resolve the selected operation or resource.
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
        CountRole,        ///< int: badge count for folder rows (0 for leaves)
        CountLabelRole,  ///< QString: what CountRole counts ("31 endpoints"), for the badge tooltip
        SessionStateRole,    ///< QString: actor rows — none/authenticating/live/refreshing
        SessionSecondsRole,  ///< int: actor rows — seconds until a live session lapses
        StatusRole,          ///< int: HTTP status of a saved-example row (0 otherwise)
        StatusTokenRole,     ///< QString: success/warning/error token for the status badge
        ProjectRootRole,     ///< QString: owning project's root path (every row carries it)
        ActiveRole,          ///< bool: true on the active project's row
        PathRole,            ///< QString: operation path template (for search matching)
    };

    /// One saved-example child row: display name and captured HTTP status (for the badge).
    struct ExampleRow {
        QString name;
        int status{0};
    };

    /// One open collection fed to the aggregated tree.
    struct ProjectEntry {
        QString root;
        QString name;
        std::shared_ptr<const engine::Project> project;
        bool active{false};
    };

    /// Composite key for `setSavedExamples`, so identically-named operations in different
    /// collections don't share rows.
    [[nodiscard]] static QString exampleKey(const QString& projectRoot, const QString& operationId);

    /// One actor's auth session as the tree renders it.
    struct ActorSessionRow {
        /// none | authenticating | live | refreshing.
        QString state{QStringLiteral("none")};
        /// Seconds until a `live` session lapses; 0 for every other state.
        int secondsRemaining{0};

        [[nodiscard]] bool operator==(const ActorSessionRow&) const = default;
    };

    /// Composite key for `setActorSessions`; sessions are per-project, so an actor name alone would
    /// blur collections.
    [[nodiscard]] static QString sessionKey(const QString& projectRoot, const QString& actorId);

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

    /// Rebuild the whole tree from every open collection (one Project node each); saved-example
    /// rows are re-applied. An empty list clears the tree.
    void populate(const std::vector<ProjectEntry>& projects);

    /// Rebuild from projects and examples in a single reset, avoiding two `beginResetModel` cycles.
    /// `examples` is keyed by `exampleKey`.
    void populate(const std::vector<ProjectEntry>& projects,
                  const QMap<QString, QList<ExampleRow>>& examples);

    /// Clear the tree (no project loaded).
    void clear();

    /// Replace the saved-example child rows, keyed by fully-qualified operation id; triggers a
    /// rebuild.
    void setSavedExamples(const QMap<QString, QList<ExampleRow>>& examplesByOperation);

    /// Replace the per-actor session snapshot, keyed by `sessionKey`. Repaints in place (no reset,
    /// so expansion and selection survive); a no-op when nothing differs, so a timer can poll it.
    void setActorSessions(QMap<QString, ActorSessionRow> sessions);

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
        QString path;  ///< operation path template (search only)
        QString exampleName;
        QString tooltip;
        QString projectRoot;  ///< owning project's root (set on every descendant)
        bool active{false};   ///< true on the active project node
        int exampleStatus{0};
        int rowInParent{0};
        Node* parent{nullptr};
        std::vector<std::unique_ptr<Node>> children;
    };

    /// The number the child-count badge shows for `node`, or 0 for rows that
    /// carry no badge.
    [[nodiscard]] static int badgeCount(const Node& node);
    /// Operation rows anywhere beneath `node` (a project's badge counts endpoints, not its group
    /// folders).
    [[nodiscard]] static int operationCount(const Node& node);
    /// Names what `badgeCount` counted ("31 endpoints", "5 actors").
    [[nodiscard]] static QString countLabel(const Node& node, int count);

    [[nodiscard]] Node* nodeFor(const QModelIndex& index) const;
    Node* addChild(Node* parent, Kind kind);
    void rebuild();

    std::unique_ptr<Node> root_;
    std::vector<ProjectEntry> projects_;
    QMap<QString, QList<ExampleRow>> examples_;
    // Keyed by sessionKey(projectRoot, actorId); held outside the node tree so a rebuild keeps live
    // state.
    QMap<QString, ActorSessionRow> actorSessions_;
};

}  // namespace reqloom::desktop::qml
