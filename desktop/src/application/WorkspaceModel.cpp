// WorkspaceModel — see header. Multi-Project Workspace Plan, Phase 1.

#include "WorkspaceModel.h"

#include "ProjectModel.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <memory>

namespace reqloom::desktop {

WorkspaceModel::WorkspaceModel(QObject* parent) : QObject(parent) {
    // Start with one project so `active()` is always valid, matching the
    // single-project lifetime the app relied on before the workspace existed.
    projects_.push_back(std::make_unique<ProjectModel>());
}

WorkspaceModel::~WorkspaceModel() = default;

ProjectModel* WorkspaceModel::active() noexcept {
    return projects_[static_cast<std::size_t>(activeIndex_)].get();
}

const ProjectModel* WorkspaceModel::active() const noexcept {
    return projects_[static_cast<std::size_t>(activeIndex_)].get();
}

int WorkspaceModel::count() const noexcept {
    return static_cast<int>(projects_.size());
}

int WorkspaceModel::activeIndex() const noexcept {
    return activeIndex_;
}

ProjectModel* WorkspaceModel::at(int index) noexcept {
    if (index < 0 || index >= count()) {
        return nullptr;
    }
    return projects_[static_cast<std::size_t>(index)].get();
}

ProjectModel* WorkspaceModel::addProject() {
    projects_.push_back(std::make_unique<ProjectModel>());
    return projects_.back().get();
}

void WorkspaceModel::setActiveIndex(int index) {
    if (index < 0 || index >= count() || index == activeIndex_) {
        return;
    }
    activeIndex_ = index;
    emit activeChanged();
}

int WorkspaceModel::indexOfRoot(const QString& rootPath) const {
    for (std::size_t i = 0; i < projects_.size(); ++i) {
        if (projects_[i]->rootPath() == rootPath) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void WorkspaceModel::removeProject(int index) {
    if (index < 0 || index >= count()) {
        return;
    }
    projects_.erase(projects_.begin() + static_cast<std::ptrdiff_t>(index));
    if (projects_.empty()) {
        // Never leave the workspace empty — a fresh sentinel keeps active()
        // valid so every hasProject() guard still holds.
        projects_.push_back(std::make_unique<ProjectModel>());
        activeIndex_ = 0;
        emit activeChanged();
        return;
    }
    if (activeIndex_ == index) {
        // The active project was removed: clamp to a surviving neighbour.
        activeIndex_ = std::min(index, count() - 1);
        emit activeChanged();
    } else if (activeIndex_ > index) {
        // Same active project, its index just shifted down — not a change.
        --activeIndex_;
    }
}

}  // namespace reqloom::desktop
