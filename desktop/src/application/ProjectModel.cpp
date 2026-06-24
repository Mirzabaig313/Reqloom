// ProjectModel — see header. Wraps engine::parseProject and exposes the
// validated Project to the views.
#include "ProjectModel.h"

#include <reqloom/engine/Factories.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <utility>

namespace reqloom::desktop {

namespace {

/// Reject names that would break the dotted id scheme (ids are joined with '.'
/// and parsed by the first dot) or escape the resources/ directory (a resource
/// name also becomes a file name). Empty is checked separately by the caller.
[[nodiscard]] bool hasIdBreakingChars(const std::string& name) {
    return name.find('.') != std::string::npos || name.find('/') != std::string::npos ||
           name.find('\\') != std::string::npos;
}

/// Rewrite every depends_on entry equal to `from` to `to` across all
/// operations, so renaming an operation keeps cross-references valid.
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

/// Rewrite depends_on entries using an old-id → new-id map in a single pass
/// (used when a resource rename re-qualifies many operation ids at once).
void remapDependencies(engine::Project& draft, const std::map<std::string, std::string>& rename) {
    for (auto& [resId, resource] : draft.resources) {
        for (auto& [opName, op] : resource.operations) {
            for (auto& dep : op.explicitDependencies) {
                if (const auto it = rename.find(dep.value); it != rename.end()) {
                    dep.value = it->second;
                }
            }
        }
    }
}

/// Re-qualify a resource's operations under `newId` (setting each op's resource
/// and fully-qualified id) and return the old-id → new-id map for remapping
/// cross-resource depends_on.
[[nodiscard]] std::map<std::string, std::string> requalifyResourceOps(
    engine::Resource& resource, const engine::ResourceId& oldId, const engine::ResourceId& newId) {
    std::map<std::string, std::string> rename;
    for (auto& [opName, op] : resource.operations) {
        op.resource = newId;
        op.id = engine::OperationId{newId.value + "." + opName};
        rename.emplace(oldId.value + "." + opName, newId.value + "." + opName);
    }
    return rename;
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

/// Remove a resource's now-orphaned `resources/<id>.yaml`. writeProject only
/// emits current resources, so a leftover file would resurrect the resource on
/// reload. Returns an error string only on a genuine removal failure (a
/// missing file is fine — `remove` reports that without setting `ec`).
[[nodiscard]] QString removeResourceFile(const std::filesystem::path& root,
                                         const std::string& resourceId) {
    std::error_code ec;
    const auto path = root / "resources" / (resourceId + ".yaml");
    std::filesystem::remove(path, ec);
    if (ec) {
        return QStringLiteral(
                   "could not remove the old file %1 (%2) — delete it manually so the "
                   "resource doesn't reappear on reload")
            .arg(QString::fromStdString(path.string()), QString::fromStdString(ec.message()));
    }
    return {};
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

    // A saved chain edit (depends_on / extract) can introduce a cycle or an
    // undefined reference; validate the draft before writing so a bad edit
    // never lands an unloadable project.
    if (auto valid = engine::validateProject(draft); !valid) {
        error = QString::fromStdString(valid.error().detail);
        return false;
    }

    auto written = engine::writeProject(root_, draft, /*overwrite=*/true);
    if (!written) {
        error = QString::fromStdString(written.error().detail);
        return false;
    }

    project_ = std::make_shared<const engine::Project>(std::move(draft));
    emit saved();
    return true;
}

bool ProjectModel::saveOperations(const std::map<std::string, engine::Operation>& updates,
                                  QString& error) {
    if (!project_) {
        error = QStringLiteral("No project loaded.");
        return false;
    }
    if (updates.empty()) {
        return true;
    }

    engine::Project draft = *project_;
    for (const auto& [idStr, updated] : updates) {
        const auto dot = idStr.find('.');
        if (dot == std::string::npos) {
            error = QStringLiteral("Malformed operation id: %1").arg(QString::fromStdString(idStr));
            return false;
        }
        const engine::ResourceId resId{idStr.substr(0, dot)};
        const auto opName = idStr.substr(dot + 1);
        auto resIt = draft.resources.find(resId);
        if (resIt == draft.resources.end() || !resIt->second.operations.contains(opName)) {
            error = QStringLiteral("Operation not found: %1").arg(QString::fromStdString(idStr));
            return false;
        }
        resIt->second.operations[opName] = updated;
    }

    // Validate the whole draft once: a chain edit can introduce a cycle or an
    // undefined reference across operations, which only a full-graph check sees.
    if (auto valid = engine::validateProject(draft); !valid) {
        error = QString::fromStdString(valid.error().detail);
        return false;
    }

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
    if (hasIdBreakingChars(trimmed)) {
        error = QStringLiteral("Name can't contain '.', '/', or '\\'.");
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
    if (hasIdBreakingChars(trimmed)) {
        error = QStringLiteral("Name can't contain '.', '/', or '\\'.");
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
    // and resource reference, then re-point cross-resource depends_on in one
    // pass via the old-id → new-id map (op-name map keys stay the same).
    engine::Resource moved = resIt->second;
    moved.id = newId;
    const auto rename = requalifyResourceOps(moved, id, newId);
    draft.resources.erase(id);
    draft.resources[newId] = std::move(moved);
    remapDependencies(draft, rename);

    auto written = engine::writeProject(root_, draft, /*overwrite=*/true);
    if (!written) {
        error = QString::fromStdString(written.error().detail);
        return false;
    }
    // Publish first so the UI reflects the rename, then surface any failure to
    // remove the now-orphaned old file (which would resurrect on reload).
    const QString removeError = removeResourceFile(root_, id.value);
    project_ = std::make_shared<const engine::Project>(std::move(draft));
    emit saved();
    if (!removeError.isEmpty()) {
        error = QStringLiteral("Renamed, but %1").arg(removeError);
        return false;
    }
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
    // Publish first so the UI reflects the delete, then surface any failure to
    // remove the now-orphaned file (which would resurrect on reload).
    const QString removeError = removeResourceFile(root_, id.value);
    project_ = std::make_shared<const engine::Project>(std::move(draft));
    emit saved();
    if (!removeError.isEmpty()) {
        error = QStringLiteral("Deleted, but %1").arg(removeError);
        return false;
    }
    return true;
}

bool ProjectModel::createResource(const QString& name, const QString& description, QString& error) {
    if (!project_) {
        error = QStringLiteral("No project loaded.");
        return false;
    }
    const std::string trimmed = name.trimmed().toStdString();
    if (trimmed.empty()) {
        error = QStringLiteral("Name cannot be empty.");
        return false;
    }
    if (hasIdBreakingChars(trimmed)) {
        error = QStringLiteral("Name can't contain '.', '/', or '\\'.");
        return false;
    }
    const engine::ResourceId id{trimmed};

    engine::Project draft = *project_;
    if (draft.resources.contains(id)) {
        error = QStringLiteral("A resource named “%1” already exists.").arg(name.trimmed());
        return false;
    }
    engine::Resource res;
    res.id = id;
    res.description = description.trimmed().toStdString();
    draft.resources[id] = std::move(res);

    // An empty resource can't break the graph, but route every mutating path
    // through the same validation gate so the invariant is uniform.
    if (auto valid = engine::validateProject(draft); !valid) {
        error = QString::fromStdString(valid.error().detail);
        return false;
    }
    auto written = engine::writeProject(root_, draft, /*overwrite=*/true);
    if (!written) {
        error = QString::fromStdString(written.error().detail);
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
    if (nameStd.empty()) {
        error = QStringLiteral("Name cannot be empty.");
        return false;
    }
    if (hasIdBreakingChars(nameStd)) {
        error = QStringLiteral("Name can't contain '.', '/', or '\\'.");
        return false;
    }
    const std::string origStd = originalId.trimmed().toStdString();
    const bool creating = origStd.empty();

    engine::Project draft = *project_;
    if (nameStd != origStd && draft.actors.contains(actor.id)) {
        error = QStringLiteral("An actor named “%1” already exists.")
                    .arg(QString::fromStdString(nameStd));
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

    if (!creating && origStd != nameStd) {
        for (auto& [resId, res] : draft.resources) {
            for (auto& [opName, op] : res.operations) {
                if (op.actor.value == origStd) {
                    op.actor = actor.id;
                }
            }
        }
    }

    if (auto valid = engine::validateProject(draft); !valid) {
        error = QString::fromStdString(valid.error().detail);
        return false;
    }
    auto written = engine::writeProject(root_, draft, /*overwrite=*/true);
    if (!written) {
        error = QString::fromStdString(written.error().detail);
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
        error = QStringLiteral("Actor “%1” not found.").arg(QString::fromStdString(id.value));
        return false;
    }
    draft.actors.erase(it);
    // No operation may point at a now-missing actor.
    for (auto& [resId, res] : draft.resources) {
        for (auto& [opName, op] : res.operations) {
            if (op.actor.value == id.value) {
                op.actor = engine::ActorId{};
            }
        }
    }
    if (auto valid = engine::validateProject(draft); !valid) {
        error = QString::fromStdString(valid.error().detail);
        return false;
    }
    auto written = engine::writeProject(root_, draft, /*overwrite=*/true);
    if (!written) {
        error = QString::fromStdString(written.error().detail);
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
    if (nameStd.empty()) {
        error = QStringLiteral("Name cannot be empty.");
        return false;
    }
    if (hasIdBreakingChars(nameStd)) {
        error = QStringLiteral("Name can't contain '.', '/', or '\\'.");
        return false;
    }
    const std::string origStd = originalName.trimmed().toStdString();
    const bool creating = origStd.empty();

    engine::Project draft = *project_;
    if (nameStd != origStd && draft.environments.contains(nameStd)) {
        error = QStringLiteral("An environment named “%1” already exists.").arg(name.trimmed());
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

    if (!creating && origStd != nameStd) {
        // Move the per-env transport config and the project default onto the
        // new name so neither dangles after the rename.
        if (const auto t = draft.transport.find(origStd); t != draft.transport.end()) {
            draft.transport[nameStd] = t->second;
            draft.transport.erase(t);
        }
        if (draft.defaultEnvironment == origStd) {
            draft.defaultEnvironment = nameStd;
        }
    }
    if (draft.defaultEnvironment.empty()) {
        draft.defaultEnvironment = nameStd;
    }

    if (auto valid = engine::validateProject(draft); !valid) {
        error = QString::fromStdString(valid.error().detail);
        return false;
    }
    auto written = engine::writeProject(root_, draft, /*overwrite=*/true);
    if (!written) {
        error = QString::fromStdString(written.error().detail);
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
    draft.environments.erase(it);
    draft.transport.erase(nameStd);
    if (draft.defaultEnvironment == nameStd) {
        draft.defaultEnvironment =
            draft.environments.empty() ? std::string{} : draft.environments.begin()->first;
    }

    if (auto valid = engine::validateProject(draft); !valid) {
        error = QString::fromStdString(valid.error().detail);
        return false;
    }
    auto written = engine::writeProject(root_, draft, /*overwrite=*/true);
    if (!written) {
        error = QString::fromStdString(written.error().detail);
        return false;
    }
    project_ = std::make_shared<const engine::Project>(std::move(draft));
    emit saved();
    return true;
}

bool ProjectModel::setLatencySloP95Ms(int ms, QString& error) {
    if (!project_) {
        error = QStringLiteral("No project loaded.");
        return false;
    }
    engine::Project draft = *project_;
    draft.latencySloP95Ms = std::max(0, ms);

    if (auto valid = engine::validateProject(draft); !valid) {
        error = QString::fromStdString(valid.error().detail);
        return false;
    }
    auto written = engine::writeProject(root_, draft, /*overwrite=*/true);
    if (!written) {
        error = QString::fromStdString(written.error().detail);
        return false;
    }
    project_ = std::make_shared<const engine::Project>(std::move(draft));
    emit saved();
    return true;
}

std::optional<engine::OperationId> ProjectModel::createOperation(
    const engine::ResourceId& resourceId,
    const QString& name,
    engine::HttpMethod method,
    const QString& pathTemplate,
    const engine::ActorId& actor,
    const std::vector<engine::OperationId>& dependencies,
    const std::vector<engine::Extraction>& extractions,
    QString& error) {
    if (!project_) {
        error = QStringLiteral("No project loaded.");
        return std::nullopt;
    }
    const std::string trimmed = name.trimmed().toStdString();
    if (trimmed.empty()) {
        error = QStringLiteral("Name cannot be empty.");
        return std::nullopt;
    }
    if (hasIdBreakingChars(trimmed)) {
        error = QStringLiteral("Name can't contain '.', '/', or '\\'.");
        return std::nullopt;
    }

    engine::Project draft = *project_;
    auto resIt = draft.resources.find(resourceId);
    if (resIt == draft.resources.end()) {
        error =
            QStringLiteral("Resource not found: %1").arg(QString::fromStdString(resourceId.value));
        return std::nullopt;
    }
    if (resIt->second.operations.contains(trimmed)) {
        error = QStringLiteral("An operation named “%1” already exists in this resource.")
                    .arg(QString::fromStdString(trimmed));
        return std::nullopt;
    }

    engine::Operation op;
    op.id = engine::OperationId{resourceId.value + "." + trimmed};
    op.resource = resourceId;
    op.actor = actor;
    op.method = method;
    op.pathTemplate = pathTemplate.trimmed().toStdString();
    op.explicitDependencies = dependencies;
    op.extractions = extractions;
    resIt->second.operations[trimmed] = std::move(op);

    // Validate the draft before writing: a dependency that forms a cycle (or an
    // undefined reference from the path template) is caught here, so a bad
    // create never lands a project that won't reload.
    if (auto valid = engine::validateProject(draft); !valid) {
        error = QString::fromStdString(valid.error().detail);
        return std::nullopt;
    }
    auto written = engine::writeProject(root_, draft, /*overwrite=*/true);
    if (!written) {
        error = QString::fromStdString(written.error().detail);
        return std::nullopt;
    }
    engine::OperationId newId{resourceId.value + "." + trimmed};
    project_ = std::make_shared<const engine::Project>(std::move(draft));
    emit saved();
    return newId;
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
