// Left-hand tree of actors and resource operations (PRD FR-5.1/5.2).
// Selecting an operation emits operationSelected; activating it (double-click
// or Enter) emits operationActivated so the shell can run it.
#pragma once

#include <chainapi/engine/Operation.h>

#include <QtWidgets/QWidget>

class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

namespace chainapi::desktop {

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

    /// Clear the tree (no project loaded).
    void clear();

signals:
    void operationSelected(QString operationId);
    void operationActivated(QString operationId);

private:
    void onSelectionChanged();
    void onItemActivated(QTreeWidgetItem* item, int column);
    void applyFilter(const QString& text);

    QLineEdit* filter_{nullptr};
    QTreeWidget* tree_{nullptr};
};

}  // namespace chainapi::desktop
