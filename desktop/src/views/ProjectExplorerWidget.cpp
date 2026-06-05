// ProjectExplorerWidget — see header. Tree view of actors + resource ops.
// Operation rows render via MethodItemDelegate: a left-aligned, colour-coded
// HTTP method badge followed by the operation name (Postman/Apidog read), all
// on one line so the verb sits at the left and moves with tree indentation.
#include "ProjectExplorerWidget.h"

#include "../application/ProjectModel.h"
#include "../widgets/MethodItemDelegate.h"
#include "../widgets/PanelHeader.h"
#include "Formatting.h"

#include <QtCore/Qt>
#include <QtGui/QColor>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QTreeWidgetItem>
#include <QtWidgets/QVBoxLayout>

#include <cstddef>

namespace reqloom::desktop {

namespace {

namespace roles = widgets::roles;

/// Recursively show rows that match `needle` (or have a matching descendant)
/// and hide the rest. Returns whether `item` ended up visible. A free helper
/// rather than a std::function so the recursion doesn't heap-allocate.
bool applyFilterTo(QTreeWidgetItem* item, const QString& needle) {
    bool anyChildVisible = false;
    for (int i = 0; i < item->childCount(); ++i) {
        anyChildVisible = applyFilterTo(item->child(i), needle) || anyChildVisible;
    }
    const bool isOperation = item->data(0, roles::kIsOperation).toBool();
    bool selfMatches = needle.isEmpty();
    if (!selfMatches && isOperation) {
        // Match on the operation id (carries the name) and the method verb.
        selfMatches =
            item->data(0, roles::kOperationId).toString().contains(needle, Qt::CaseInsensitive) ||
            item->data(0, roles::kMethodText).toString().contains(needle, Qt::CaseInsensitive);
    }
    const bool visible = selfMatches || anyChildVisible;
    item->setHidden(!visible);
    return visible;
}

/// Re-tint one operation row's stored method colour (and recurse) so a runtime
/// theme switch updates the delegate-painted badges.
void recolorMethodsIn(QTreeWidgetItem* item, const theming::Theme& theme) {
    const QString method = item->data(0, roles::kMethodText).toString();
    if (!method.isEmpty()) {
        item->setData(0, roles::kMethodColor, theme.method(format::methodColor(method)));
    }
    for (int i = 0; i < item->childCount(); ++i) {
        recolorMethodsIn(item->child(i), theme);
    }
}

}  // namespace

ProjectExplorerWidget::ProjectExplorerWidget(QWidget* parent) : QWidget(parent) {
    // Deepest surface + a sensible floor so actor/operation names never
    // truncate to "ad…" when the splitter is at its default position.
    setObjectName(QStringLiteral("explorerPanel"));
    setAttribute(Qt::WA_StyledBackground, true);
    setMinimumWidth(220);

    auto* layout = new QVBoxLayout(this);
    const int gap = theming::Theme::space(theming::Space::Sm);
    layout->setContentsMargins(gap, gap, gap, gap);
    layout->setSpacing(gap);

    header_ = new widgets::PanelHeader(QStringLiteral("Explorer"), this);
    header_->setSubtitle(QStringLiteral("No project open"));
    auto* collapseBtn = new QToolButton(this);
    collapseBtn->setObjectName(QStringLiteral("railButton"));
    collapseBtn->setText(QStringLiteral("‹"));
    collapseBtn->setAutoRaise(true);
    collapseBtn->setToolTip(QStringLiteral("Collapse sidebar (Cmd+B)"));
    collapseBtn->setAccessibleName(QStringLiteral("Collapse sidebar"));
    connect(collapseBtn, &QToolButton::clicked, this, [this]() { emit collapseRequested(); });
    header_->addTrailingWidget(collapseBtn);
    layout->addWidget(header_);

    filter_ = new QLineEdit(this);
    filter_->setPlaceholderText(QStringLiteral("Filter operations…"));
    filter_->setClearButtonEnabled(true);
    filter_->setAccessibleName(QStringLiteral("Filter operations"));
    layout->addWidget(filter_);

    // Single-column tree: the method badge is painted inside the row by the
    // delegate (left of the name), not in a separate column.
    tree_ = new QTreeWidget(this);
    tree_->setObjectName(QStringLiteral("explorerTree"));
    tree_->setColumnCount(1);
    tree_->setHeaderHidden(true);
    tree_->setIndentation(14);
    tree_->setUniformRowHeights(true);
    methodDelegate_ = new widgets::MethodItemDelegate(this);
    tree_->setItemDelegate(methodDelegate_);
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
        header_->setTitle(QStringLiteral("Explorer"));
        header_->setSubtitle(QStringLiteral("No project open"));
        return;
    }
    const auto& proj = project.project();

    // Surface the project name as the explorer's header (the reference's
    // "<API name>" title), with the operation count as the secondary line.
    std::size_t opCount = 0;
    for (const auto& [resId, resource] : proj.resources) {
        opCount += resource.operations.size();
    }
    const QString projectName = project.name();
    header_->setTitle(projectName.isEmpty() ? QStringLiteral("Explorer") : projectName);
    header_->setSubtitle(
        QStringLiteral("%1 operations · %2 actors").arg(opCount).arg(proj.actors.size()));

    auto* actorsRoot = new QTreeWidgetItem(tree_);
    actorsRoot->setText(0, QStringLiteral("📁  Actors"));
    actorsRoot->setData(0, roles::kIsOperation, false);
    for (const auto& [actorId, actor] : proj.actors) {
        auto* actorItem = new QTreeWidgetItem(actorsRoot);
        const QString name = QString::fromStdString(actorId.value);
        actorItem->setText(0, name);
        actorItem->setToolTip(0, name);  // full name on hover (names truncate)
        actorItem->setData(0, roles::kIsOperation, false);
    }

    auto* resourcesRoot = new QTreeWidgetItem(tree_);
    resourcesRoot->setText(0, QStringLiteral("📁  Resources"));
    resourcesRoot->setData(0, roles::kIsOperation, false);
    for (const auto& [resId, resource] : proj.resources) {
        auto* resItem = new QTreeWidgetItem(resourcesRoot);
        const QString resName = QString::fromStdString(resId.value);
        resItem->setText(0, QStringLiteral("📂  %1").arg(resName));
        resItem->setToolTip(0, resName);
        resItem->setData(0, roles::kIsOperation, false);
        for (const auto& [opName, op] : resource.operations) {
            auto* opItem = new QTreeWidgetItem(resItem);
            const QString name = QString::fromStdString(opName);
            const QString method = format::method(op.method);
            // The delegate paints the badge + name; the row text holds the
            // name so the default fallback and accessibility stay sensible.
            opItem->setText(0, name);
            opItem->setToolTip(0,
                               QStringLiteral("%1\n%2 %3")
                                   .arg(QString::fromStdString(op.id.value),
                                        method,
                                        QString::fromStdString(op.pathTemplate)));
            opItem->setData(0, roles::kIsOperation, true);
            opItem->setData(0, roles::kOperationId, QString::fromStdString(op.id.value));
            opItem->setData(0, roles::kMethodText, method);
            opItem->setData(0, roles::kMethodColor, theme_.method(format::methodColor(method)));
        }
    }

    tree_->expandAll();
}

void ProjectExplorerWidget::applyTheme(const theming::Theme& theme) {
    theme_ = theme;
    header_->setTheme(theme);
    methodDelegate_->setTheme(theme);
    recolorMethods();
    tree_->viewport()->update();
}

void ProjectExplorerWidget::recolorMethods() {
    // Refresh each operation row's stored method colour from the current theme.
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        recolorMethodsIn(tree_->topLevelItem(i), theme_);
    }
}

void ProjectExplorerWidget::onSelectionChanged() {
    const auto items = tree_->selectedItems();
    if (items.isEmpty()) {
        return;
    }
    auto* item = items.first();
    if (item->data(0, roles::kIsOperation).toBool()) {
        emit operationSelected(item->data(0, roles::kOperationId).toString());
    }
}

void ProjectExplorerWidget::onItemActivated(QTreeWidgetItem* item, int /*column*/) {
    if (item != nullptr && item->data(0, roles::kIsOperation).toBool()) {
        emit operationActivated(item->data(0, roles::kOperationId).toString());
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

}  // namespace reqloom::desktop
