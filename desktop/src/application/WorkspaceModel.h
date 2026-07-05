// WorkspaceModel — owns the set of open projects (collections) and tracks which
// one is active. The active project is what the rest of the desktop edits and
// runs; it is never null (the workspace is constructed with one project and
// always keeps at least that one), so callers' `hasProject()` guards stay
// valid. Multi-Project Workspace Plan, Phase 1.
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

    /// The active project. Never null: the workspace always holds at least the
    /// project it was constructed with.
    [[nodiscard]] ProjectModel* active() noexcept;
    [[nodiscard]] const ProjectModel* active() const noexcept;

    [[nodiscard]] int count() const noexcept;
    [[nodiscard]] int activeIndex() const noexcept;

    /// The project at `index`, or nullptr when out of range.
    [[nodiscard]] ProjectModel* at(int index) noexcept;

    /// Append a new, empty project (owned here) and return it. Does not change
    /// the active index — the caller decides when to activate it.
    ProjectModel* addProject();

    /// Make `index` the active project. Out-of-range indices are ignored.
    /// Emits `activeChanged` only when the active index actually moves.
    void setActiveIndex(int index);

    /// Index of the open project loaded from `rootPath`, or -1 if none. Lets a
    /// caller activate an already-open collection instead of opening a
    /// duplicate. Matching is by exact `rootPath()` string (callers pass a
    /// canonical path).
    [[nodiscard]] int indexOfRoot(const QString& rootPath) const;

    /// Remove the project at `index`. Preserves the invariant that the
    /// workspace always holds at least one project: removing the last one
    /// leaves a fresh empty project. Adjusts the active index and emits
    /// `activeChanged` when the active project changes as a result.
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
