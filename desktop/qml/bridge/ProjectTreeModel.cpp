// ProjectTreeModel — see header.
#include "ProjectTreeModel.h"

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
        case ExampleNameRole:
            return node->exampleName;
        case TooltipRole:
            return node->tooltip.isEmpty() ? node->name : node->tooltip;
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
        {ExampleNameRole, "exampleName"},
        {TooltipRole, "tooltip"},
    };
}

void ProjectTreeModel::populate(const engine::Project& project) {
    project_ = std::make_shared<const engine::Project>(project);
    rebuild();
}

void ProjectTreeModel::clear() {
    project_.reset();
    rebuild();
}

void ProjectTreeModel::setSavedExamples(const QMap<QString, QStringList>& examplesByOperation) {
    examples_ = examplesByOperation;
    rebuild();
}

void ProjectTreeModel::rebuild() {
    beginResetModel();
    root_ = std::make_unique<Node>();
    if (project_) {
        const engine::Project& proj = *project_;

        Node* actorsRoot = addChild(root_.get(), Kind::ActorGroup);
        actorsRoot->name = QStringLiteral("Actors");
        for (const auto& [actorId, actor] : proj.actors) {
            Node* actorNode = addChild(actorsRoot, Kind::Actor);
            actorNode->name = QString::fromStdString(actorId.value);
            actorNode->tooltip = actorNode->name;
        }

        Node* resourcesRoot = addChild(root_.get(), Kind::ResourceGroup);
        resourcesRoot->name = QStringLiteral("Resources");
        for (const auto& [resId, resource] : proj.resources) {
            Node* resNode = addChild(resourcesRoot, Kind::Resource);
            resNode->name = QString::fromStdString(resId.value);
            resNode->resourceId = resNode->name;
            resNode->tooltip = resNode->name;
            for (const auto& [opName, op] : resource.operations) {
                Node* opNode = addChild(resNode, Kind::Operation);
                const QString opId = QString::fromStdString(op.id.value);
                opNode->name = QString::fromStdString(opName);
                opNode->operationId = opId;
                opNode->method = methodLabel(op.method);
                opNode->tooltip =
                    QStringLiteral("%1\n%2 %3")
                        .arg(opId, opNode->method, QString::fromStdString(op.pathTemplate));
                const auto exIt = examples_.constFind(opId);
                if (exIt != examples_.constEnd()) {
                    for (const QString& exampleName : exIt.value()) {
                        Node* exNode = addChild(opNode, Kind::Example);
                        exNode->name = exampleName;
                        exNode->exampleName = exampleName;
                        exNode->operationId = opId;
                        exNode->tooltip = QStringLiteral("Saved example — click to load");
                    }
                }
            }
        }
    }
    endResetModel();
}

}  // namespace reqloom::desktop::qml
