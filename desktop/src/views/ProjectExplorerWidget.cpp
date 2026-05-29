// ProjectExplorerWidget — see header. Tree view of actors + resource ops.
#include "ProjectExplorerWidget.h"

#include "../application/ProjectModel.h"
#include "Formatting.h"

#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QTreeWidgetItem>
#include <QtWidgets/QVBoxLayout>

namespace chainapi::desktop {

namespace {

// Roles for stashing the fully-qualified operation id on a tree row.
constexpr int kOperationIdRole = Qt::UserRole + 1;
// Marks a row as an operation (vs. a category/actor/resource header).
constexpr int kIsOperationRole = Qt::UserRole + 2;

/// Recursively show rows that match `needle` (or have a matching descendant)
/// and hide the rest. Returns whether `item` ended up visible. A free helper
/// rather than a std::function so the recursion doesn't heap-allocate.
bool applyFilterTo(QTreeWidgetItem* item, const QString& needle) {
    bool anyChildVisible = false;
    for (int i = 0; i < item->childCount(); ++i) {
        anyChildVisible = applyFilterTo(item->child(i), needle) || anyChildVisible;
    }
    const bool isOperation = item->data(0, kIsOperationRole).toBool();
    bool selfMatches = needle.isEmpty();
    if (!selfMatches && isOperation) {
        selfMatches = item->text(0).contains(needle, Qt::CaseInsensitive) ||
                      item->text(1).contains(needle, Qt::CaseInsensitive);
    }
    const bool visible = selfMatches || anyChildVisible;
    item->setHidden(!visible);
    return visible;
}

}  // namespace

ProjectExplorerWidget::ProjectExplorerWidget(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    filter_ = new QLineEdit(this);
    filter_->setPlaceholderText(QStringLiteral("Filter operations…"));
    filter_->setClearButtonEnabled(true);
    layout->addWidget(filter_);

    tree_ = new QTreeWidget(this);
    tree_->setColumnCount(2);
    tree_->setHeaderLabels({QStringLiteral("Name"), QStringLiteral("Method")});
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tree_->setUniformRowHeights(true);
    layout->addWidget(tree_, 1);

    connect(tree_,
            &QTreeWidget::itemSelectionChanged,
            this,
            &ProjectExplorerWidget::onSelectionChanged);
    connect(tree_, &QTreeWidget::itemActivated, this, &ProjectExplorerWidget::onItemActivated);
    connect(filter_, &QLineEdit::textChanged, this, &ProjectExplorerWidget::applyFilter);
}

ProjectExplorerWidget::~ProjectExplorerWidget() = default;

void ProjectExplorerWidget::clear() {
    tree_->clear();
}

void ProjectExplorerWidget::populate(const ProjectModel& project) {
    tree_->clear();
    if (!project.hasProject()) {
        return;
    }
    const auto& proj = project.project();

    auto* actorsRoot = new QTreeWidgetItem(tree_);
    actorsRoot->setText(0, QStringLiteral("Actors"));
    actorsRoot->setData(0, kIsOperationRole, false);
    actorsRoot->setFirstColumnSpanned(true);
    for (const auto& [actorId, actor] : proj.actors) {
        auto* actorItem = new QTreeWidgetItem(actorsRoot);
        actorItem->setText(0, QString::fromStdString(actorId.value));
        actorItem->setData(0, kIsOperationRole, false);
    }

    auto* resourcesRoot = new QTreeWidgetItem(tree_);
    resourcesRoot->setText(0, QStringLiteral("Resources"));
    resourcesRoot->setData(0, kIsOperationRole, false);
    resourcesRoot->setFirstColumnSpanned(true);
    for (const auto& [resId, resource] : proj.resources) {
        auto* resItem = new QTreeWidgetItem(resourcesRoot);
        resItem->setText(0, QString::fromStdString(resId.value));
        resItem->setData(0, kIsOperationRole, false);
        for (const auto& [opName, op] : resource.operations) {
            auto* opItem = new QTreeWidgetItem(resItem);
            opItem->setText(0, QString::fromStdString(opName));
            opItem->setText(1, format::method(op.method));
            opItem->setData(0, kIsOperationRole, true);
            opItem->setData(0, kOperationIdRole, QString::fromStdString(op.id.value));
        }
    }

    tree_->expandAll();
}

void ProjectExplorerWidget::onSelectionChanged() {
    const auto items = tree_->selectedItems();
    if (items.isEmpty()) {
        return;
    }
    auto* item = items.first();
    if (item->data(0, kIsOperationRole).toBool()) {
        emit operationSelected(item->data(0, kOperationIdRole).toString());
    }
}

void ProjectExplorerWidget::onItemActivated(QTreeWidgetItem* item, int /*column*/) {
    if (item != nullptr && item->data(0, kIsOperationRole).toBool()) {
        emit operationActivated(item->data(0, kOperationIdRole).toString());
    }
}

void ProjectExplorerWidget::applyFilter(const QString& text) {
    const QString needle = text.trimmed();
    // Walk every operation leaf; hide non-matches and any now-empty parents.
    // A category/resource stays visible if at least one descendant matches.
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        applyFilterTo(tree_->topLevelItem(i), needle);
    }
}

}  // namespace chainapi::desktop
