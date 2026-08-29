// ProjectTreeModel — see header.
#include "ProjectTreeModel.h"

#include <functional>
#include <numeric>
#include <utility>

namespace reqloom::desktop::qml {

namespace {

[[nodiscard]] QString methodLabel(engine::HttpMethod method) {
    switch (method) {
        case engine::HttpMethod::Get:
            return QStringLiteral("GET");
        case engine::HttpMethod::Post:
            return QStringLiteral("POST");
        case engine::HttpMethod::Put:
            return QStringLiteral("PUT");
        case engine::HttpMethod::Patch:
            return QStringLiteral("PATCH");
        case engine::HttpMethod::Delete:
            return QStringLiteral("DELETE");
        case engine::HttpMethod::Head:
            return QStringLiteral("HEAD");
        case engine::HttpMethod::Options:
            return QStringLiteral("OPTIONS");
    }
    return QStringLiteral("GET");
}

}  // namespace

ProjectTreeModel::ProjectTreeModel(QObject* parent)
    : QAbstractItemModel(parent), root_(std::make_unique<Node>()) {}

ProjectTreeModel::~ProjectTreeModel() = default;

ProjectTreeModel::Node* ProjectTreeModel::nodeFor(const QModelIndex& index) const {
    if (!index.isValid()) {
        return root_.get();
    }
    return static_cast<Node*>(index.internalPointer());
}

ProjectTreeModel::Node* ProjectTreeModel::addChild(Node* parent, Kind kind) {
    auto child = std::make_unique<Node>();
    child->kind = kind;
    child->parent = parent;
    child->rowInParent = static_cast<int>(parent->children.size());
    Node* raw = child.get();
    parent->children.push_back(std::move(child));
    return raw;
}

QModelIndex ProjectTreeModel::index(int row, int column, const QModelIndex& parent) const {
    if (column != 0 || row < 0) {
        return {};
    }
    Node* parentNode = nodeFor(parent);
    if (parentNode == nullptr || row >= static_cast<int>(parentNode->children.size())) {
        return {};
    }
    return createIndex(row, column, parentNode->children[static_cast<std::size_t>(row)].get());
}

QModelIndex ProjectTreeModel::parent(const QModelIndex& child) const {
    if (!child.isValid()) {
        return {};
    }
    Node* node = nodeFor(child);
    if (node == nullptr || node->parent == nullptr || node->parent == root_.get()) {
        return {};
    }
    Node* parentNode = node->parent;
    return createIndex(parentNode->rowInParent, 0, parentNode);
}

int ProjectTreeModel::rowCount(const QModelIndex& parent) const {
    if (parent.column() > 0) {
        return 0;
    }
    Node* node = nodeFor(parent);
    return node == nullptr ? 0 : static_cast<int>(node->children.size());
}

int ProjectTreeModel::columnCount(const QModelIndex& /*parent*/) const {
    return 1;
}

QVariant ProjectTreeModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid()) {
        return {};
    }
    Node* node = nodeFor(index);
    if (node == nullptr) {
        return {};
    }
    switch (role) {
        case KindRole:
            switch (node->kind) {
                case Kind::Project:
                    return QStringLiteral("project");
                case Kind::ActorGroup:
                    return QStringLiteral("actorGroup");
                case Kind::ResourceGroup:
                    return QStringLiteral("resourceGroup");
                case Kind::Actor:
                    return QStringLiteral("actor");
                case Kind::Resource:
                    return QStringLiteral("resource");
                case Kind::Operation:
                    return QStringLiteral("operation");
                case Kind::Example:
                    return QStringLiteral("example");
            }
            return {};
        case NameRole:
        case Qt::DisplayRole:
            return node->name;
        case OperationIdRole:
            return node->operationId;
        case ResourceIdRole:
            return node->resourceId;
        case MethodRole:
            return node->method;
        case PathRole:
            return node->path;
        case ExampleNameRole:
            return node->exampleName;
        case TooltipRole:
            return node->tooltip.isEmpty() ? node->name : node->tooltip;
        case ProjectRootRole:
            return node->projectRoot;
        case ActiveRole:
            return node->active;
        case CountRole:
            return badgeCount(*node);
        case CountLabelRole:
            return countLabel(*node, badgeCount(*node));
        case SessionStateRole:
            if (node->kind != Kind::Actor) {
                return QString{};
            }
            return actorSessions_.value(sessionKey(node->projectRoot, node->name)).state;
        case SessionSecondsRole:
            if (node->kind != Kind::Actor) {
                return 0;
            }
            return actorSessions_.value(sessionKey(node->projectRoot, node->name)).secondsRemaining;
        case StatusRole:
            return node->exampleStatus;
        case StatusTokenRole: {
            const int s = node->exampleStatus;
            if (s >= 200 && s < 300) {
                return QStringLiteral("success");
            }
            if (s >= 300 && s < 400) {
                return QStringLiteral("warning");
            }
            if (s >= 400) {
                return QStringLiteral("error");
            }
            return QString{};
        }
        default:
            return {};
    }
}

int ProjectTreeModel::operationCount(const Node& node) {
    if (node.kind == Kind::Operation) {
        return 1;
    }
    return std::transform_reduce(
        node.children.begin(),
        node.children.end(),
        0,
        std::plus<>{},
        [](const std::unique_ptr<Node>& child) { return operationCount(*child); });
}

int ProjectTreeModel::badgeCount(const Node& node) {
    switch (node.kind) {
        case Kind::Project:
            // A project's direct children are the two group folders, so "2"
            // says nothing. Count the endpoints underneath instead.
            return operationCount(node);
        case Kind::ActorGroup:
        case Kind::ResourceGroup:
        case Kind::Resource:
            return static_cast<int>(node.children.size());
        default:
            // Leaves (operations, actors, examples) carry no badge.
            return 0;
    }
}

QString ProjectTreeModel::countLabel(const Node& node, int count) {
    if (count <= 0) {
        return {};
    }
    // Singular and plural spelled out rather than tr("%n unit(s)"): Qt only
    // picks a plural form when a translator is loaded, and untranslated builds
    // would render the literal "1 endpoint(s)".
    switch (node.kind) {
        case Kind::Project:
        case Kind::Resource:
            return count == 1 ? tr("1 endpoint") : tr("%1 endpoints").arg(count);
        case Kind::ActorGroup:
            return count == 1 ? tr("1 actor") : tr("%1 actors").arg(count);
        case Kind::ResourceGroup:
            return count == 1 ? tr("1 resource") : tr("%1 resources").arg(count);
        default:
            return {};
    }
}

QHash<int, QByteArray> ProjectTreeModel::roleNames() const {
    return {
        {KindRole, "kind"},
        {NameRole, "name"},
        {OperationIdRole, "operationId"},
        {ResourceIdRole, "resourceId"},
        {MethodRole, "method"},
        {PathRole, "opPath"},
        {ExampleNameRole, "exampleName"},
        {TooltipRole, "tooltip"},
        {CountRole, "count"},
        {CountLabelRole, "countLabel"},
        {SessionStateRole, "sessionState"},
        {SessionSecondsRole, "sessionSeconds"},
        {StatusRole, "status"},
        {StatusTokenRole, "statusToken"},
        {ProjectRootRole, "projectRoot"},
        {ActiveRole, "active"},
    };
}

QString ProjectTreeModel::exampleKey(const QString& projectRoot, const QString& operationId) {
    // U+001F (unit separator) can't appear in a filesystem root or an op id.
    return projectRoot + QChar(u'\x1f') + operationId;
}

QString ProjectTreeModel::sessionKey(const QString& projectRoot, const QString& actorId) {
    return projectRoot + QChar(u'\x1f') + actorId;
}

void ProjectTreeModel::setActorSessions(QMap<QString, ActorSessionRow> sessions) {
    if (sessions == actorSessions_) {
        return;
    }
    actorSessions_ = std::move(sessions);

    // Actor rows are always the first group under a project.
    for (int p = 0; p < rowCount({}); ++p) {
        const QModelIndex actors = index(0, 0, index(p, 0, {}));
        const int actorRows = rowCount(actors);
        if (actorRows == 0) {
            continue;
        }
        emit dataChanged(index(0, 0, actors),
                         index(actorRows - 1, 0, actors),
                         {SessionStateRole, SessionSecondsRole});
    }
}

void ProjectTreeModel::populate(const std::vector<ProjectEntry>& projects) {
    projects_ = projects;
    rebuild();
}

void ProjectTreeModel::populate(const std::vector<ProjectEntry>& projects,
                                const QMap<QString, QList<ExampleRow>>& examples) {
    projects_ = projects;
    examples_ = examples;
    rebuild();
}

void ProjectTreeModel::clear() {
    projects_.clear();
    rebuild();
}

void ProjectTreeModel::setSavedExamples(
    const QMap<QString, QList<ExampleRow>>& examplesByOperation) {
    examples_ = examplesByOperation;
    rebuild();
}

void ProjectTreeModel::rebuild() {
    beginResetModel();
    root_ = std::make_unique<Node>();
    for (const ProjectEntry& entry : projects_) {
        if (!entry.project) {
            continue;
        }
        const engine::Project& proj = *entry.project;

        // One Project node per open collection; its subtree carries the owning
        // root on every descendant so a selection resolves the right project.
        Node* projectNode = addChild(root_.get(), Kind::Project);
        projectNode->name = entry.name.isEmpty() ? entry.root : entry.name;
        projectNode->projectRoot = entry.root;
        projectNode->active = entry.active;
        projectNode->tooltip = entry.root;

        Node* actorsRoot = addChild(projectNode, Kind::ActorGroup);
        actorsRoot->name = QStringLiteral("Actors");
        actorsRoot->projectRoot = entry.root;
        for (const auto& [actorId, actor] : proj.actors) {
            Node* actorNode = addChild(actorsRoot, Kind::Actor);
            actorNode->name = QString::fromStdString(actorId.value);
            actorNode->projectRoot = entry.root;
            actorNode->tooltip = actorNode->name;
        }

        Node* resourcesRoot = addChild(projectNode, Kind::ResourceGroup);
        resourcesRoot->name = QStringLiteral("Resources");
        resourcesRoot->projectRoot = entry.root;
        for (const auto& [resId, resource] : proj.resources) {
            Node* resNode = addChild(resourcesRoot, Kind::Resource);
            resNode->name = QString::fromStdString(resId.value);
            resNode->resourceId = resNode->name;
            resNode->projectRoot = entry.root;
            resNode->tooltip = resNode->name;
            for (const auto& [opName, op] : resource.operations) {
                Node* opNode = addChild(resNode, Kind::Operation);
                const QString opId = QString::fromStdString(op.id.value);
                opNode->name = QString::fromStdString(opName);
                opNode->operationId = opId;
                opNode->projectRoot = entry.root;
                opNode->method = methodLabel(op.method);
                opNode->path = QString::fromStdString(op.pathTemplate);
                opNode->tooltip =
                    QStringLiteral("%1\n%2 %3").arg(opId, opNode->method, opNode->path);
                const auto exIt = examples_.constFind(exampleKey(entry.root, opId));
                if (exIt != examples_.constEnd()) {
                    for (const ExampleRow& example : exIt.value()) {
                        Node* exNode = addChild(opNode, Kind::Example);
                        exNode->name = example.name;
                        exNode->exampleName = example.name;
                        exNode->operationId = opId;
                        exNode->projectRoot = entry.root;
                        exNode->exampleStatus = example.status;
                        exNode->tooltip = QStringLiteral("Saved example — click to load");
                    }
                }
            }
        }
    }
    endResetModel();
}

}  // namespace reqloom::desktop::qml
