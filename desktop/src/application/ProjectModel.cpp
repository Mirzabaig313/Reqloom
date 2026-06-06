// ProjectModel — see header. Wraps engine::parseProject and exposes the
// validated Project to the views.
#include "ProjectModel.h"

#include <reqloom/engine/Factories.h>

#include <algorithm>
#include <filesystem>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace reqloom::desktop {

namespace {

/// Rewrite every depends_on entry equal to `from` to `to` across all
/// operations, so renaming an operation/resource keeps cross-references valid.
void remapDependencies(engine::Project& draft, const std::string& from, const std::string& to) {
    for (auto& [resId, resource] : draft.resources) {
        for (auto& [opName, op] : resource.operations) {
            for (auto& dep : op.explicitDependencies) {
                if (dep.value == from) {
                    dep.value = to;
                }
            }
        }
    }
}

/// Drop every depends_on entry whose target is in `removed`, so deleting an
/// operation/resource doesn't leave dangling references that fail to parse.
void dropDependencies(engine::Project& draft, const std::set<std::string>& removed) {
    for (auto& [resId, resource] : draft.resources) {
        for (auto& [opName, op] : resource.operations) {
            std::erase_if(op.explicitDependencies, [&removed](const engine::OperationId& dep) {
                return removed.contains(dep.value);
            });
        }
    }
}

}  // namespace

ProjectModel::ProjectModel(QObject* parent) : QObject(parent) {}

ProjectModel::~ProjectModel() = default;

void ProjectModel::loadFromDirectory(const QString& directory) {
    const std::filesystem::path dir{directory.toStdString()};
    const auto yaml = dir / "reqloom.yaml";

    auto parsed = engine::parseProject(yaml);
    if (!parsed) {
        const auto& err = parsed.error();
        const auto codeStr = engine::toCodeString(err.code);
        emit loadFailed(QString::fromUtf8(codeStr.data(), static_cast<qsizetype>(codeStr.size())),
                        QString::fromStdString(err.detail));
        return;
    }

    project_ = std::make_shared<const engine::Project>(std::move(*parsed));
    root_ = dir;
    emit loaded();
}

bool ProjectModel::hasProject() const noexcept {
    return project_ != nullptr;
}

const engine::Project& ProjectModel::project() const noexcept {
    return *project_;
}

std::shared_ptr<const engine::Project> ProjectModel::projectPtr() const noexcept {
    return project_;
}

QString ProjectModel::name() const {
    return project_ ? QString::fromStdString(project_->name) : QString{};
}

QString ProjectModel::rootPath() const {
    return root_.empty() ? QString{} : QString::fromStdString(root_.string());
}

QStringList ProjectModel::environmentNames() const {
    QStringList names;
    if (!project_) {
        return names;
    }
    // Default environment first so the UI's combo defaults sensibly.
    const auto& def = project_->defaultEnvironment;
    if (!def.empty() && project_->environments.contains(def)) {
        names.append(QString::fromStdString(def));
    }
    for (const auto& [envName, _] : project_->environments) {
        const auto qName = QString::fromStdString(envName);
        if (!names.contains(qName)) {
            names.append(qName);
        }
    }
    return names;
}

QString ProjectModel::defaultEnvironment() const {
    return project_ ? QString::fromStdString(project_->defaultEnvironment) : QString{};
}

bool ProjectModel::saveOperation(const engine::OperationId& id,
                                 const engine::Operation& updated,
                                 QString& error) {
    if (!project_) {
        error = QStringLiteral("No project loaded.");
        return false;
    }
    const auto dot = id.value.find('.');
    if (dot == std::string::npos) {
        error = QStringLiteral("Malformed operation id: %1").arg(QString::fromStdString(id.value));
        return false;
    }
    const engine::ResourceId resId{id.value.substr(0, dot)};
    const auto opName = id.value.substr(dot + 1);

    // Edit a copy, persist it, then publish the copy only if the write
    // succeeds. On write failure project_ keeps the last good in-memory state;
    // the on-disk files may be partially written (writeProject is not atomic),
    // so the caller surfaces the error and the user can retry or reload.
    engine::Project draft = *project_;
    auto resIt = draft.resources.find(resId);
    if (resIt == draft.resources.end() || !resIt->second.operations.contains(opName)) {
        error = QStringLiteral("Operation not found: %1").arg(QString::fromStdString(id.value));
        return false;
    }
    resIt->second.operations[opName] = updated;

    auto written = engine::writeProject(root_, draft, /*overwrite=*/true);
    if (!written) {
        error = QString::fromStdString(written.error().detail);
        return false;
    }

    project_ = std::make_shared<const engine::Project>(std::move(draft));
    emit saved();
    return true;
}

bool ProjectModel::renameOperation(const engine::OperationId& id,
                                   const QString& newName,
                                   QString& error) {
    if (!project_) {
        error = QStringLiteral("No project loaded.");
        return false;
    }
    const std::string trimmed = newName.trimmed().toStdString();
    if (trimmed.empty()) {
        error = QStringLiteral("Name cannot be empty.");
        return false;
    }
    const auto dot = id.value.find('.');
    if (dot == std::string::npos) {
        error = QStringLiteral("Malformed operation id: %1").arg(QString::fromStdString(id.value));
        return false;
    }
    const engine::ResourceId resId{id.value.substr(0, dot)};
    const auto opName = id.value.substr(dot + 1);
    if (trimmed == opName) {
        return true;  // no change
    }

    engine::Project draft = *project_;
    auto resIt = draft.resources.find(resId);
    if (resIt == draft.resources.end() || !resIt->second.operations.contains(opName)) {
        error = QStringLiteral("Operation not found: %1").arg(QString::fromStdString(id.value));
        return false;
    }
    if (resIt->second.operations.contains(trimmed)) {
        error = QStringLiteral("An operation named “%1” already exists in this resource.")
                    .arg(QString::fromStdString(trimmed));
        return false;
    }

    // Move the operation under its new name, updating its fully-qualified id.
    engine::Operation moved = resIt->second.operations.at(opName);
    moved.id = engine::OperationId{resId.value + "." + trimmed};
    resIt->second.operations.erase(opName);
    resIt->second.operations[trimmed] = std::move(moved);
    // Keep other operations' depends_on pointing at the renamed operation.
    remapDependencies(draft, id.value, resId.value + "." + trimmed);

    auto written = engine::writeProject(root_, draft, /*overwrite=*/true);
    if (!written) {
        error = QString::fromStdString(written.error().detail);
        return false;
    }
    project_ = std::make_shared<const engine::Project>(std::move(draft));
    emit saved();
    return true;
}

bool ProjectModel::deleteOperation(const engine::OperationId& id, QString& error) {
    if (!project_) {
        error = QStringLiteral("No project loaded.");
        return false;
    }
    const auto dot = id.value.find('.');
    if (dot == std::string::npos) {
        error = QStringLiteral("Malformed operation id: %1").arg(QString::fromStdString(id.value));
        return false;
    }
    const engine::ResourceId resId{id.value.substr(0, dot)};
    const auto opName = id.value.substr(dot + 1);

    engine::Project draft = *project_;
    auto resIt = draft.resources.find(resId);
    if (resIt == draft.resources.end() || !resIt->second.operations.contains(opName)) {
        error = QStringLiteral("Operation not found: %1").arg(QString::fromStdString(id.value));
        return false;
    }
    resIt->second.operations.erase(opName);
    // Drop any other operation's now-dangling depends_on entry for it.
    dropDependencies(draft, {id.value});

    auto written = engine::writeProject(root_, draft, /*overwrite=*/true);
    if (!written) {
        error = QString::fromStdString(written.error().detail);
        return false;
    }
    project_ = std::make_shared<const engine::Project>(std::move(draft));
    emit saved();
    return true;
}

bool ProjectModel::renameResource(const engine::ResourceId& id,
                                  const QString& newName,
                                  QString& error) {
    if (!project_) {
        error = QStringLiteral("No project loaded.");
        return false;
    }
    const std::string trimmed = newName.trimmed().toStdString();
    if (trimmed.empty()) {
        error = QStringLiteral("Name cannot be empty.");
        return false;
    }
    if (trimmed == id.value) {
        return true;  // no change
    }
    const engine::ResourceId newId{trimmed};

    engine::Project draft = *project_;
    auto resIt = draft.resources.find(id);
    if (resIt == draft.resources.end()) {
        error = QStringLiteral("Resource not found: %1").arg(QString::fromStdString(id.value));
        return false;
    }
    if (draft.resources.contains(newId)) {
        error = QStringLiteral("A resource named “%1” already exists.").arg(newName.trimmed());
        return false;
    }

    // Move the resource under its new id, re-qualifying every operation's id
    // and resource reference (op map keys — the op-name parts — stay the same).
    engine::Resource moved = resIt->second;
    moved.id = newId;
    for (auto& [opName, op] : moved.operations) {
        op.resource = newId;
        op.id = engine::OperationId{newId.value + "." + opName};
    }
    // Collect the op-name parts so we can re-point cross-resource depends_on
    // from "<old>.<op>" to "<new>.<op>" after the move.
    std::vector<std::string> opNames;
    opNames.reserve(moved.operations.size());
    for (const auto& [opName, op] : moved.operations) {
        opNames.push_back(opName);
    }
    draft.resources.erase(id);
    draft.resources[newId] = std::move(moved);
    for (const auto& opName : opNames) {
        remapDependencies(draft, id.value + "." + opName, newId.value + "." + opName);
    }

    auto written = engine::writeProject(root_, draft, /*overwrite=*/true);
    if (!written) {
        error = QString::fromStdString(written.error().detail);
        return false;
    }
    // writeProject only emits current resources, so the old file lingers and
    // would resurrect the resource on reload — remove it (best effort).
    std::error_code ec;
    std::filesystem::remove(root_ / "resources" / (id.value + ".yaml"), ec);

    project_ = std::make_shared<const engine::Project>(std::move(draft));
    emit saved();
    return true;
}

bool ProjectModel::deleteResource(const engine::ResourceId& id, QString& error) {
    if (!project_) {
        error = QStringLiteral("No project loaded.");
        return false;
    }
    engine::Project draft = *project_;
    auto resIt = draft.resources.find(id);
    if (resIt == draft.resources.end()) {
        error = QStringLiteral("Resource not found: %1").arg(QString::fromStdString(id.value));
        return false;
    }
    // Gather the resource's operation ids so cross-resource depends_on entries
    // pointing into it can be dropped (they'd otherwise dangle on reload).
    std::set<std::string> removedOps;
    for (const auto& [opName, op] : resIt->second.operations) {
        removedOps.insert(id.value + "." + opName);
    }
    draft.resources.erase(resIt);
    dropDependencies(draft, removedOps);

    auto written = engine::writeProject(root_, draft, /*overwrite=*/true);
    if (!written) {
        error = QString::fromStdString(written.error().detail);
        return false;
    }
    // Remove the resource's now-orphaned file so it doesn't reappear on reload.
    std::error_code ec;
    std::filesystem::remove(root_ / "resources" / (id.value + ".yaml"), ec);

    project_ = std::make_shared<const engine::Project>(std::move(draft));
    emit saved();
    return true;
}

const engine::Operation* ProjectModel::findOperation(const engine::OperationId& id) const noexcept {
    if (!project_) {
        return nullptr;
    }
    const auto dot = id.value.find('.');
    if (dot == std::string::npos) {
        return nullptr;
    }
    const engine::ResourceId resId{id.value.substr(0, dot)};
    const auto opName = id.value.substr(dot + 1);

    const auto resIt = project_->resources.find(resId);
    if (resIt == project_->resources.end()) {
        return nullptr;
    }
    const auto opIt = resIt->second.operations.find(opName);
    if (opIt == resIt->second.operations.end()) {
        return nullptr;
    }
    return &opIt->second;
}

}  // namespace reqloom::desktop
