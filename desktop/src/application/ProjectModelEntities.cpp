// ProjectModel — create, rename and delete of operations, resources, actors and environments.
// Every path edits a draft, validates it with the engine and writes it before publishing.
#include "ProjectEdits.h"
#include "ProjectModel.h"

#include <reqloom/engine/Factories.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <ranges>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace reqloom::desktop {

namespace {

namespace fs = std::filesystem;

[[nodiscard]] QString qs(const std::string& text) {
    return QString::fromStdString(text);
}

template <typename Map>
[[nodiscard]] std::vector<std::string> namesOf(const Map& map) {
    std::vector<std::string> names;
    for (const auto& key : map | std::views::keys) {
        if constexpr (requires { key.value; }) {
            names.push_back(key.value);
        } else {
            names.push_back(key);
        }
    }
    return names;
}

/// Why `name` can't name a `kind` beside `existing` (`self` is the entity being renamed), or empty.
[[nodiscard]] QString nameProblem(const QString& kind,
                                  const std::string& name,
                                  const std::vector<std::string>& existing,
                                  const std::string& self = {}) {
    if (!isValidEntityName(name)) {
        return invalidNameMessage();
    }
    if (const auto clash = caseInsensitiveClash(existing, name, self)) {
        if (*clash == name) {
            return QStringLiteral("%1 named “%2” already exists.").arg(kind, qs(name));
        }
        return QStringLiteral(
                   "%1 named “%2” already exists; names can't differ only in letter case.")
            .arg(kind, qs(*clash));
    }
    return {};
}

[[nodiscard]] bool differOnlyInCase(const std::string& a, const std::string& b) {
    return a != b && qs(a).compare(qs(b), Qt::CaseInsensitive) == 0;
}

[[nodiscard]] bool validDraft(const engine::Project& draft, QString& error) {
    if (auto valid = engine::validateProject(draft); !valid) {
        error = qs(valid.error().detail);
        return false;
    }
    return true;
}

/// Rename `<dir>/<from>.yaml` to `<to>.yaml` through a temporary name, so a change of letter case
/// also takes effect on a case-insensitive volume. A missing file is fine.
[[nodiscard]] QString renameFileCase(const fs::path& dir,
                                     const std::string& from,
                                     const std::string& to) {
    std::error_code ec;
    const auto source = dir / (from + ".yaml");
    if (!fs::exists(source, ec)) {
        return {};
    }
    const auto temp = dir / (to + ".yaml.case-rename");
    fs::rename(source, temp, ec);
    if (!ec) {
        fs::rename(temp, dir / (to + ".yaml"), ec);
        if (ec) {
            std::error_code back;
            fs::rename(temp, source, back);
        }
    }
    if (ec) {
        return QStringLiteral("could not rename %1 (%2)")
            .arg(qs(source.string()), qs(ec.message()));
    }
    return {};
}

/// Remove a renamed or deleted resource's `resources/<oldId>.yaml` unless it is the file now saved
/// as `<newId>.yaml`. Returns an error only on a genuine failure; a missing file is fine.
[[nodiscard]] QString removeResourceFile(const fs::path& root,
                                         const std::string& oldId,
                                         const std::string& newId = {}) {
    std::error_code ec;
    const auto path = root / "resources" / (oldId + ".yaml");
    if (!newId.empty() && fs::equivalent(path, root / "resources" / (newId + ".yaml"), ec)) {
        return {};
    }
    ec.clear();
    fs::remove(path, ec);
    if (ec) {
        return QStringLiteral(
                   "could not remove the old file %1 (%2) — delete it manually so the "
                   "resource doesn't reappear on reload")
            .arg(qs(path.string()), qs(ec.message()));
    }
    return {};
}

void remapDependencies(engine::Project& draft, const std::map<std::string, std::string>& rename) {
    for (auto& resource : draft.resources | std::views::values) {
        for (auto& op : resource.operations | std::views::values) {
            for (auto& dep : op.explicitDependencies) {
                if (const auto it = rename.find(dep.value); it != rename.end()) {
                    dep.value = it->second;
                }
            }
        }
    }
}

/// Operations outside `removed` whose depends_on names one of `removed`, sorted.
[[nodiscard]] QStringList dependentsOn(const engine::Project& project,
                                       const std::set<std::string>& removed) {
    QStringList out;
    for (const auto& [resId, resource] : project.resources) {
        for (const auto& [opName, op] : resource.operations) {
            const auto id = resId.value + "." + opName;
            if (!removed.contains(id) &&
                std::ranges::any_of(op.explicitDependencies, [&](const engine::OperationId& dep) {
                    return removed.contains(dep.value);
                })) {
                out << qs(id);
            }
        }
    }
    return out;
}

void dropDependencies(engine::Project& draft, const std::set<std::string>& removed) {
    for (auto& resource : draft.resources | std::views::values) {
        for (auto& op : resource.operations | std::views::values) {
            std::erase_if(op.explicitDependencies, [&removed](const engine::OperationId& dep) {
                return removed.contains(dep.value);
            });
        }
    }
}

[[nodiscard]] std::set<std::string> operationIdsOf(const engine::Resource& resource) {
    std::set<std::string> ids;
    for (const auto& opName : resource.operations | std::views::keys) {
        ids.insert(resource.id.value + "." + opName);
    }
    return ids;
}

/// References that lose their only producer when `opName` of `resource` is deleted.
[[nodiscard]] std::vector<EntityReference> orphanedConsumers(const engine::Project& project,
                                                             const engine::Resource& resource,
                                                             const std::string& opName) {
    const auto selfId = resource.id.value + "." + opName;
    std::vector<EntityReference> out;
    for (const auto& extraction : resource.operations.at(opName).extractions) {
        const bool otherProducer = std::ranges::any_of(resource.operations, [&](const auto& entry) {
            return entry.first != opName &&
                   std::ranges::any_of(entry.second.extractions, [&](const engine::Extraction& e) {
                       return e.variableName == extraction.variableName;
                   });
        });
        if (otherProducer) {
            continue;
        }
        for (auto& ref : findScopeReferences(project, resource.id.value, extraction.variableName)) {
            if (ref.owner != selfId) {
                out.push_back(std::move(ref));
            }
        }
    }
    return out;
}

/// Where a template or for_each outside the resource itself still refers to resource `id`.
[[nodiscard]] std::vector<EntityReference> resourceReferences(const engine::Project& project,
                                                              const engine::ResourceId& id) {
    const auto ownPrefix = id.value + ".";
    std::vector<EntityReference> out;
    for (auto& ref : findScopeReferences(project, id.value)) {
        if (!ref.owner.starts_with(ownPrefix)) {
            out.push_back(std::move(ref));
        }
    }
    for (const auto& [resId, resource] : project.resources) {
        for (const auto& [opName, op] : resource.operations) {
            if (resId != id && op.forEach && op.forEach->over == id) {
                out.push_back({resId.value + "." + opName, "for_each"});
            }
        }
    }
    return out;
}

/// Where a template of another entity or a poll_until still refers to actor `id`.
[[nodiscard]] std::vector<EntityReference> actorReferences(const engine::Project& project,
                                                           const engine::ActorId& id) {
    const auto self = "actor " + id.value;
    std::vector<EntityReference> out;
    for (auto& ref : findScopeReferences(project, id.value)) {
        if (ref.owner != self) {
            out.push_back(std::move(ref));
        }
    }
    for (const auto& [resId, resource] : project.resources) {
        for (const auto& [opName, op] : resource.operations) {
            if (op.pollUntil && op.pollUntil->actor && *op.pollUntil->actor == id) {
                out.push_back({resId.value + "." + opName, "poll_until actor"});
            }
        }
    }
    return out;
}

[[nodiscard]] std::optional<std::pair<engine::ResourceId, std::string>> splitOperationId(
    const engine::OperationId& id) {
    const auto dot = id.value.find('.');
    if (dot == std::string::npos) {
        return std::nullopt;
    }
    return std::pair{engine::ResourceId{id.value.substr(0, dot)}, id.value.substr(dot + 1)};
}

}  // namespace

bool ProjectModel::validateAndWrite(const engine::Project& draft, QString& error) const {
    if (!validDraft(draft, error)) {
        return false;
    }
    auto written = engine::writeProject(root_, draft, /*overwrite=*/true);
    if (!written) {
        error = qs(written.error().detail);
        return false;
    }
    return true;
}

bool ProjectModel::writeRenamed(const engine::Project& draft,
                                const std::string& folder,
                                const std::string& from,
                                const std::string& to,
                                QString& error) const {
    if (!differOnlyInCase(from, to)) {
        return validateAndWrite(draft, error);
    }
    if (!validDraft(draft, error)) {
        return false;
    }
    // The writer would save the same file under the new name and then prune it as stale.
    const auto dir = root_ / folder;
    if (const QString moved = renameFileCase(dir, from, to); !moved.isEmpty()) {
        error = moved;
        return false;
    }
    auto written = engine::writeProject(root_, draft, /*overwrite=*/true);
    if (!written) {
        (void)renameFileCase(dir, to, from);
        error = qs(written.error().detail);
        return false;
    }
    return true;
}

bool ProjectModel::renameOperation(const engine::OperationId& id,
                                   const QString& newName,
                                   QString& error) {
    if (!project_) {
        error = QStringLiteral("No project loaded.");
        return false;
    }
    const auto parts = splitOperationId(id);
    if (!parts) {
        error = QStringLiteral("Malformed operation id: %1").arg(qs(id.value));
        return false;
    }
    const auto& [resId, opName] = *parts;
    const std::string trimmed = newName.trimmed().toStdString();
    if (trimmed == opName) {
        return true;
    }

    engine::Project draft = *project_;
    auto resIt = draft.resources.find(resId);
    if (resIt == draft.resources.end() || !resIt->second.operations.contains(opName)) {
        error = QStringLiteral("Operation not found: %1").arg(qs(id.value));
        return false;
    }
    if (const QString problem = nameProblem(
            QStringLiteral("An operation"), trimmed, namesOf(resIt->second.operations), opName);
        !problem.isEmpty()) {
        error = problem;
        return false;
    }

    const engine::OperationId newId{resId.value + "." + trimmed};
    engine::Operation moved = resIt->second.operations.at(opName);
    moved.id = newId;
    resIt->second.operations.erase(opName);
    resIt->second.operations[trimmed] = std::move(moved);
    remapDependencies(draft, {{id.value, newId.value}});

    if (!validateAndWrite(draft, error)) {
        return false;
    }
    project_ = std::make_shared<const engine::Project>(std::move(draft));
    emit operationRenamed(qs(id.value), qs(newId.value));
    emit saved();
    return true;
}

bool ProjectModel::deleteOperation(const engine::OperationId& id,
                                   QString& error,
                                   QStringList* droppedFrom) {
    if (!project_) {
        error = QStringLiteral("No project loaded.");
        return false;
    }
    const auto parts = splitOperationId(id);
    if (!parts) {
        error = QStringLiteral("Malformed operation id: %1").arg(qs(id.value));
        return false;
    }
    const auto& [resId, opName] = *parts;

    engine::Project draft = *project_;
    auto resIt = draft.resources.find(resId);
    if (resIt == draft.resources.end() || !resIt->second.operations.contains(opName)) {
        error = QStringLiteral("Operation not found: %1").arg(qs(id.value));
        return false;
    }
    if (const auto orphans = orphanedConsumers(draft, resIt->second, opName); !orphans.empty()) {
        error = QStringLiteral(
                    "“%1” is the only endpoint that extracts a value still used by: %2. "
                    "Remove those references first.")
                    .arg(qs(id.value), describeReferences(orphans));
        return false;
    }
    const QStringList dependents = dependentsOn(draft, {id.value});
    resIt->second.operations.erase(opName);
    dropDependencies(draft, {id.value});

    if (!validateAndWrite(draft, error)) {
        return false;
    }
    project_ = std::make_shared<const engine::Project>(std::move(draft));
    if (droppedFrom != nullptr) {
        *droppedFrom = dependents;
    }
    emit operationRemoved(qs(id.value));
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
    if (trimmed == id.value) {
        return true;
    }
    engine::Project draft = *project_;
    auto resIt = draft.resources.find(id);
    if (resIt == draft.resources.end()) {
        error = QStringLiteral("Resource not found: %1").arg(qs(id.value));
        return false;
    }
    if (const QString problem =
            nameProblem(QStringLiteral("A resource"), trimmed, namesOf(draft.resources), id.value);
        !problem.isEmpty()) {
        error = problem;
        return false;
    }
    const engine::ResourceId newId{trimmed};

    engine::Resource moved = std::move(resIt->second);
    draft.resources.erase(resIt);
    moved.id = newId;
    std::map<std::string, std::string> rename;
    for (auto& [opName, op] : moved.operations) {
        op.resource = newId;
        op.id = engine::OperationId{newId.value + "." + opName};
        rename.emplace(id.value + "." + opName, op.id.value);
    }
    draft.resources[newId] = std::move(moved);
    remapDependencies(draft, rename);
    for (auto& resource : draft.resources | std::views::values) {
        for (auto& op : resource.operations | std::views::values) {
            if (op.forEach && op.forEach->over == id) {
                op.forEach->over = newId;
            }
        }
    }
    renameScopeInProject(draft, id.value, newId.value);

    if (!writeRenamed(draft, "resources", id.value, newId.value, error)) {
        return false;
    }
    const QString removeError = removeResourceFile(root_, id.value, newId.value);
    project_ = std::make_shared<const engine::Project>(std::move(draft));
    for (const auto& [oldOp, newOp] : rename) {
        emit operationRenamed(qs(oldOp), qs(newOp));
    }
    emit saved();
    if (!removeError.isEmpty()) {
        error = QStringLiteral("Renamed, but %1").arg(removeError);
        return false;
    }
    return true;
}

bool ProjectModel::deleteResource(const engine::ResourceId& id,
                                  QString& error,
                                  QStringList* droppedFrom) {
    if (!project_) {
        error = QStringLiteral("No project loaded.");
        return false;
    }
    engine::Project draft = *project_;
    auto resIt = draft.resources.find(id);
    if (resIt == draft.resources.end()) {
        error = QStringLiteral("Resource not found: %1").arg(qs(id.value));
        return false;
    }
    if (const auto refs = resourceReferences(draft, id); !refs.empty()) {
        error = QStringLiteral("Module “%1” is still used by: %2. Remove those references first.")
                    .arg(qs(id.value), describeReferences(refs));
        return false;
    }
    const std::set<std::string> removedOps = operationIdsOf(resIt->second);
    const QStringList dependents = dependentsOn(draft, removedOps);
    draft.resources.erase(resIt);
    dropDependencies(draft, removedOps);

    if (!validateAndWrite(draft, error)) {
        return false;
    }
    const QString removeError = removeResourceFile(root_, id.value);
    project_ = std::make_shared<const engine::Project>(std::move(draft));
    if (droppedFrom != nullptr) {
        *droppedFrom = dependents;
    }
    for (const auto& opId : removedOps) {
        emit operationRemoved(qs(opId));
    }
    emit saved();
    if (!removeError.isEmpty()) {
        error = QStringLiteral("Deleted, but %1").arg(removeError);
        return false;
    }
    return true;
}

QStringList ProjectModel::dependentsOf(const QString& kind, const QString& id) const {
    if (!project_) {
        return {};
    }
    const std::string idStd = id.toStdString();
    if (kind == QLatin1String("operation")) {
        return dependentsOn(*project_, {idStd});
    }
    if (kind == QLatin1String("resource")) {
        const auto it = project_->resources.find(engine::ResourceId{idStd});
        return it == project_->resources.end()
                   ? QStringList{}
                   : dependentsOn(*project_, operationIdsOf(it->second));
    }
    QStringList out;
    if (kind == QLatin1String("actor")) {
        for (const auto& resource : project_->resources | std::views::values) {
            for (const auto& op : resource.operations | std::views::values) {
                if (op.actor.value == idStd) {
                    out << qs(op.id.value);
                }
            }
        }
    }
    return out;
}

bool ProjectModel::createResource(const QString& name, const QString& description, QString& error) {
    if (!project_) {
        error = QStringLiteral("No project loaded.");
        return false;
    }
    const std::string trimmed = name.trimmed().toStdString();
    engine::Project draft = *project_;
    if (const QString problem =
            nameProblem(QStringLiteral("A resource"), trimmed, namesOf(draft.resources));
        !problem.isEmpty()) {
        error = problem;
        return false;
    }
    const engine::ResourceId id{trimmed};
    engine::Resource res;
    res.id = id;
    res.description = description.trimmed().toStdString();
    draft.resources[id] = std::move(res);

    if (!validateAndWrite(draft, error)) {
        return false;
    }
    project_ = std::make_shared<const engine::Project>(std::move(draft));
    emit saved();
    return true;
}

bool ProjectModel::saveActor(const QString& originalId,
                             const engine::Actor& actor,
                             QString& error) {
    if (!project_) {
        error = QStringLiteral("No project loaded.");
        return false;
    }
    const std::string nameStd = actor.id.value;
    const std::string origStd = originalId.trimmed().toStdString();
    const bool creating = origStd.empty();

    engine::Project draft = *project_;
    if (const QString problem =
            nameProblem(QStringLiteral("An actor"), nameStd, namesOf(draft.actors), origStd);
        !problem.isEmpty()) {
        error = problem;
        return false;
    }
    if (!creating) {
        const auto it = draft.actors.find(engine::ActorId{origStd});
        if (it == draft.actors.end()) {
            error = QStringLiteral("Actor “%1” not found.").arg(originalId.trimmed());
            return false;
        }
        draft.actors.erase(it);
    }
    draft.actors[actor.id] = actor;

    const bool renaming = !creating && origStd != nameStd;
    if (renaming) {
        for (auto& resource : draft.resources | std::views::values) {
            for (auto& op : resource.operations | std::views::values) {
                if (op.actor.value == origStd) {
                    op.actor = actor.id;
                }
                if (op.pollUntil && op.pollUntil->actor && op.pollUntil->actor->value == origStd) {
                    op.pollUntil->actor = actor.id;
                }
            }
        }
        renameScopeInProject(draft, origStd, nameStd);
    }

    if (!(renaming ? writeRenamed(draft, "actors", origStd, nameStd, error)
                   : validateAndWrite(draft, error))) {
        return false;
    }
    project_ = std::make_shared<const engine::Project>(std::move(draft));
    emit saved();
    return true;
}

bool ProjectModel::deleteActor(const engine::ActorId& id, QString& error) {
    if (!project_) {
        error = QStringLiteral("No project loaded.");
        return false;
    }
    engine::Project draft = *project_;
    const auto it = draft.actors.find(id);
    if (it == draft.actors.end()) {
        error = QStringLiteral("Actor “%1” not found.").arg(qs(id.value));
        return false;
    }
    if (const auto refs = actorReferences(draft, id); !refs.empty()) {
        error = QStringLiteral("Actor “%1” is still used by: %2. Remove those references first.")
                    .arg(qs(id.value), describeReferences(refs));
        return false;
    }
    draft.actors.erase(it);
    for (auto& resource : draft.resources | std::views::values) {
        for (auto& op : resource.operations | std::views::values) {
            if (op.actor.value == id.value) {
                op.actor = engine::ActorId{};
            }
        }
    }
    if (!validateAndWrite(draft, error)) {
        return false;
    }
    project_ = std::make_shared<const engine::Project>(std::move(draft));
    emit saved();
    return true;
}

bool ProjectModel::saveEnvironment(const QString& originalName,
                                   const QString& name,
                                   const std::map<std::string, std::string>& variables,
                                   QString& error) {
    if (!project_) {
        error = QStringLiteral("No project loaded.");
        return false;
    }
    const std::string nameStd = name.trimmed().toStdString();
    const std::string origStd = originalName.trimmed().toStdString();
    const bool creating = origStd.empty();

    engine::Project draft = *project_;
    if (const QString problem = nameProblem(
            QStringLiteral("An environment"), nameStd, namesOf(draft.environments), origStd);
        !problem.isEmpty()) {
        error = problem;
        return false;
    }
    if (!creating) {
        const auto it = draft.environments.find(origStd);
        if (it == draft.environments.end()) {
            error = QStringLiteral("Environment “%1” not found.").arg(originalName.trimmed());
            return false;
        }
        draft.environments.erase(it);
    }
    draft.environments[nameStd] = variables;

    const bool renaming = !creating && origStd != nameStd;
    if (renaming) {
        if (const auto t = draft.transport.find(origStd); t != draft.transport.end()) {
            auto config = std::move(t->second);
            draft.transport.erase(t);
            draft.transport[nameStd] = std::move(config);
        }
        if (draft.defaultEnvironment == origStd) {
            draft.defaultEnvironment = nameStd;
        }
    }
    if (draft.defaultEnvironment.empty()) {
        draft.defaultEnvironment = nameStd;
    }

    if (!(renaming ? writeRenamed(draft, "environments", origStd, nameStd, error)
                   : validateAndWrite(draft, error))) {
        return false;
    }
    project_ = std::make_shared<const engine::Project>(std::move(draft));
    emit saved();
    return true;
}

bool ProjectModel::deleteEnvironment(const QString& name, QString& error) {
    if (!project_) {
        error = QStringLiteral("No project loaded.");
        return false;
    }
    const std::string nameStd = name.trimmed().toStdString();
    engine::Project draft = *project_;
    const auto it = draft.environments.find(nameStd);
    if (it == draft.environments.end()) {
        error = QStringLiteral("Environment “%1” not found.").arg(name.trimmed());
        return false;
    }
    if (draft.environments.size() == 1) {
        error = QStringLiteral("“%1” is the only environment; add another before deleting it.")
                    .arg(name.trimmed());
        return false;
    }
    draft.environments.erase(it);
    draft.transport.erase(nameStd);
    if (draft.defaultEnvironment == nameStd) {
        draft.defaultEnvironment = draft.environments.begin()->first;
    }

    if (!validateAndWrite(draft, error)) {
        return false;
    }
    project_ = std::make_shared<const engine::Project>(std::move(draft));
    emit saved();
    return true;
}

std::optional<engine::OperationId> ProjectModel::createOperation(
    const engine::ResourceId& resourceId,
    const QString& name,
    const engine::Operation& operation,
    QString& error) {
    if (!project_) {
        error = QStringLiteral("No project loaded.");
        return std::nullopt;
    }
    const std::string trimmed = name.trimmed().toStdString();
    engine::Project draft = *project_;
    auto resIt = draft.resources.find(resourceId);
    if (!isValidEntityName(trimmed)) {
        error = invalidNameMessage();
        return std::nullopt;
    }
    if (resIt == draft.resources.end()) {
        error = QStringLiteral("Resource not found: %1").arg(qs(resourceId.value));
        return std::nullopt;
    }
    if (const QString problem =
            nameProblem(QStringLiteral("An operation"), trimmed, namesOf(resIt->second.operations));
        !problem.isEmpty()) {
        error = problem;
        return std::nullopt;
    }

    const engine::OperationId newId{resourceId.value + "." + trimmed};
    engine::Operation complete = operation;
    complete.id = newId;
    complete.resource = resourceId;
    resIt->second.operations[trimmed] = std::move(complete);

    if (!validateAndWrite(draft, error)) {
        return std::nullopt;
    }
    project_ = std::make_shared<const engine::Project>(std::move(draft));
    emit saved();
    return newId;
}

}  // namespace reqloom::desktop
