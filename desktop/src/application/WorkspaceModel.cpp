// WorkspaceModel — see header.

#include "WorkspaceModel.h"

#include "ProjectModel.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <memory>

namespace reqloom::desktop {

WorkspaceModel::WorkspaceModel(QObject* parent) : QObject(parent) {
    // Start with one project so `active()` is always valid.
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
        // Never leave the workspace empty: a fresh sentinel keeps active() valid.
        projects_.push_back(std::make_unique<ProjectModel>());
        activeIndex_ = 0;
        emit activeChanged();
        return;
    }
    if (activeIndex_ == index) {
        activeIndex_ = std::min(index, count() - 1);
        emit activeChanged();
    } else if (activeIndex_ > index) {
        // Same active project with its index shifted down: not a change.
        --activeIndex_;
    }
}

void WorkspaceModel::markUnavailable(const QString& path,
                                     const QString& name,
                                     const QString& reason) {
    for (UnavailableProject& entry : unavailable_) {
        if (entry.path == path) {
            entry.reason = reason;
            if (!name.isEmpty()) {
                entry.name = name;
            }
            return;
        }
    }
    unavailable_.append(UnavailableProject{.path = path, .name = name, .reason = reason});
}

bool WorkspaceModel::forgetUnavailable(const QString& path) {
    return unavailable_.removeIf([&](const UnavailableProject& e) { return e.path == path; }) > 0;
}

bool WorkspaceModel::isUnavailable(const QString& path) const {
    return std::ranges::any_of(unavailable_,
                               [&](const UnavailableProject& e) { return e.path == path; });
}

QList<WorkspaceModel::SavedProject> WorkspaceModel::savedProjects() const {
    QList<SavedProject> saved;
    for (const auto& project : projects_) {
        if (project->hasProject()) {
            saved.append(SavedProject{.path = project->rootPath(), .name = project->name()});
        }
    }
    for (const UnavailableProject& entry : unavailable_) {
        if (indexOfRoot(entry.path) < 0) {
            saved.append(SavedProject{.path = entry.path, .name = entry.name});
        }
    }
    return saved;
}

}  // namespace reqloom::desktop
