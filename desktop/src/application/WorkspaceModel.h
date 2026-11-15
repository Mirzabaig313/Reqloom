// WorkspaceModel — owns the open projects (collections) and tracks the active one.
// The active project is never null: the workspace always keeps at least one.
#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>

#include <memory>
#include <vector>

namespace reqloom::desktop {

class ProjectModel;

class WorkspaceModel : public QObject {
    Q_OBJECT

public:
    explicit WorkspaceModel(QObject* parent = nullptr);
    ~WorkspaceModel() override;

    WorkspaceModel(const WorkspaceModel&) = delete;
    WorkspaceModel& operator=(const WorkspaceModel&) = delete;
    WorkspaceModel(WorkspaceModel&&) = delete;
    WorkspaceModel& operator=(WorkspaceModel&&) = delete;

    /// The active project. Never null.
    [[nodiscard]] ProjectModel* active() noexcept;
    [[nodiscard]] const ProjectModel* active() const noexcept;

    [[nodiscard]] int count() const noexcept;
    [[nodiscard]] int activeIndex() const noexcept;

    /// The project at `index`, or nullptr when out of range.
    [[nodiscard]] ProjectModel* at(int index) noexcept;

    /// Append a new, empty project (owned here); does not change the active index.
    ProjectModel* addProject();

    /// Make `index` the active project. Out-of-range indices are ignored.
    /// Emits `activeChanged` only when the active index actually moves.
    void setActiveIndex(int index);

    /// Index of the open project loaded from `rootPath` (exact string match; callers pass a
    /// canonical path), or -1.
    [[nodiscard]] int indexOfRoot(const QString& rootPath) const;

    /// Remove the project at `index`; removing the last leaves a fresh empty one. Emits
    /// `activeChanged` if the active project changes.
    void removeProject(int index);

signals:
    /// Emitted when the active project changes so consumers rebind their
    /// per-project views. Not fired on a no-op `setActiveIndex`.
    void activeChanged();

private:
    std::vector<std::unique_ptr<ProjectModel>> projects_;
    int activeIndex_{0};
};

}  // namespace reqloom::desktop
