// Left-hand tree of actors and resource operations (PRD FR-5.1/5.2).
// Selecting an operation emits operationSelected; activating it (double-click
// or Enter) emits operationActivated so the shell can run it.
#pragma once

#include "../theming/Theme.h"

#include <reqloom/engine/Operation.h>

#include <QtCore/QHash>
#include <QtWidgets/QWidget>

class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

namespace reqloom::desktop {

namespace widgets {
class PanelHeader;
class MethodItemDelegate;
}  // namespace widgets

class ProjectModel;

class ProjectExplorerWidget : public QWidget {
    Q_OBJECT

public:
    explicit ProjectExplorerWidget(QWidget* parent = nullptr);
    ~ProjectExplorerWidget() override;

    ProjectExplorerWidget(const ProjectExplorerWidget&) = delete;
    ProjectExplorerWidget& operator=(const ProjectExplorerWidget&) = delete;
    ProjectExplorerWidget(ProjectExplorerWidget&&) = delete;
    ProjectExplorerWidget& operator=(ProjectExplorerWidget&&) = delete;

    /// Rebuild the tree from the loaded project.
    void populate(const ProjectModel& project);

    /// Set the saved-example child rows under an operation (Apidog-style
    /// examples nested below the endpoint). Replaces any existing example rows.
    void setSavedExamples(const QString& operationId, const QStringList& exampleNames);

    /// Adopt a new theme: re-tint method chips and refresh fonts.
    void applyTheme(const theming::Theme& theme);

    /// Clear the tree (no project loaded).
    void clear();

signals:
    void operationSelected(QString operationId);
    void operationActivated(QString operationId);
    /// The user clicked the header's collapse control; the shell hides this
    /// panel and shows the thin rail.
    void collapseRequested();
    /// A saved-example row was selected; the shell loads the operation and
    /// shows that stored response.
    void exampleSelected(QString operationId, QString exampleName);
    /// Context-menu actions on a saved-example row.
    void exampleRenameRequested(QString operationId, QString exampleName);
    void exampleDuplicateRequested(QString operationId, QString exampleName);
    void exampleDeleteRequested(QString operationId, QString exampleName);
    /// Context-menu actions on an operation (endpoint) row.
    void operationEditRequested(QString operationId);
    void operationRenameRequested(QString operationId);
    void operationDeleteRequested(QString operationId);
    /// Context-menu actions on a resource (folder) row.
    void resourceRenameRequested(QString resourceId);
    void resourceDeleteRequested(QString resourceId);
    /// Create actions. `operationCreateRequested` carries the resource to
    /// create under, or an empty string to let the dialog pick.
    void resourceCreateRequested();
    void operationCreateRequested(QString resourceId);

private:
    void onSelectionChanged();
    void onItemActivated(QTreeWidgetItem* item, int column);
    void onContextMenu(const QPoint& pos);
    void applyFilter(const QString& text);
    void recolorMethods();

    QLineEdit* filter_{nullptr};
    QTreeWidget* tree_{nullptr};
    widgets::PanelHeader* header_{nullptr};
    widgets::MethodItemDelegate* methodDelegate_{nullptr};
    // Operation id → its tree item, so saved-example children can be attached.
    QHash<QString, QTreeWidgetItem*> opItems_;
    theming::Theme theme_{theming::Theme::resolve(theming::Appearance::Dark)};
};

}  // namespace reqloom::desktop
