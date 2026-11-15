// ProjectModel — see header. Wraps engine::parseProject.
#include "ProjectModel.h"

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/ProjectIdentity.h>

#include <QtCore/QByteArray>
#include <QtCore/QFile>
#include <QtCore/QSaveFile>

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace reqloom::desktop {

namespace {

/// Reject names that would break the dotted id scheme or escape resources/ (a resource name is also
/// a file name). Empty is checked by the caller.
[[nodiscard]] bool hasIdBreakingChars(const std::string& name) {
    return name.find('.') != std::string::npos || name.find('/') != std::string::npos ||
           name.find('\\') != std::string::npos;
}

/// Rewrite every depends_on entry equal to `from` to `to`, so renaming an operation keeps
/// references valid.
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

/// Same, using an old-id → new-id map in one pass (for a resource rename).
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

/// Re-qualify a resource's operations under `newId`; returns the old-id → new-id map for
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

/// Drop depends_on entries targeting `removed`, so deletes leave no dangling references.
void dropDependencies(engine::Project& draft, const std::set<std::string>& removed) {
    for (auto& [resId, resource] : draft.resources) {
        for (auto& [opName, op] : resource.operations) {
            std::erase_if(op.explicitDependencies, [&removed](const engine::OperationId& dep) {
                return removed.contains(dep.value);
            });
        }
    }
}

/// Remove a resource's orphaned `resources/<id>.yaml` (writeProject only emits current ones, so it
/// would resurrect on reload). Returns an error only on a genuine failure; a missing file is fine.
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

[[nodiscard]] bool writeAtomically(const std::filesystem::path& path, const QByteArray& bytes) {
    QSaveFile out{QString::fromStdU16String(path.u16string())};
    return out.open(QIODevice::WriteOnly) && out.write(bytes) == bytes.size() && out.commit();
}

/// Insert a single `id:` line into reqloom.yaml, leaving every other byte intact.
/// Restores the original text and returns nullopt if the edit or re-parse fails.
[[nodiscard]] std::optional<engine::Project> insertIdInPlace(const std::filesystem::path& yaml) {
    QFile in{QString::fromStdU16String(yaml.u16string())};
    if (!in.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    const QByteArray original = in.readAll();
    in.close();

    const auto id = engine::generateProjectId();
    const auto edited = engine::insertProjectIdLine(
        std::string_view{original.constData(), static_cast<std::size_t>(original.size())}, id);
    if (!edited || !edited->has_value()) {
        return std::nullopt;
    }
    if (!writeAtomically(yaml, QByteArray::fromStdString(**edited))) {
        return std::nullopt;
    }
    auto reparsed = engine::parseProject(yaml);
    if (reparsed && reparsed->id == id) {
        return std::move(*reparsed);
    }
    (void)writeAtomically(yaml, original);
    return std::nullopt;
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

    // Secrets are keyed by project id, so persist one before any secret is stored; if unsafe, open
    // without an id.
    if (parsed->id.empty()) {
        if (auto withId = insertIdInPlace(yaml)) {
            parsed = std::move(*withId);
        }
    }

    project_ = std::make_shared<const engine::Project>(std::move(*parsed));
    root_ = dir;
    emit loaded();
}

bool ProjectModel::createProject(const QString& directory, const QString& name, QString& error) {
    const std::filesystem::path dir{directory.toStdString()};

    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) {
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            error = QStringLiteral("Couldn't create the project folder: %1")
                        .arg(QString::fromStdString(ec.message()));
            return false;
        }
    }

    engine::Project draft;
    const std::string trimmed = name.trimmed().toStdString();
    // A blank name falls back to the folder name; the writer round-trips it into `name:`.
    draft.name = trimmed.empty() ? dir.filename().string() : trimmed;

    // overwrite=false: re-scaffolding over a real project errors instead of losing data; the caller
    // shows the message.
    auto written = engine::writeProject(dir, draft, /*overwrite=*/false);
    if (!written) {
        error = QString::fromStdString(written.error().detail);
        return false;
    }

    // Reuse the normal load path so `loaded` fires; root_ is only set on a successful parse, so it
    // doubles as the success signal.
    loadFromDirectory(directory);
    return root_ == dir;
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

    // Edit a copy, persist it, then publish only if the write succeeds; on failure project_ keeps
    // the last good state (writeProject is not atomic, so disk may be partial; the caller surfaces
    // the error).
    engine::Project draft = *project_;
    auto resIt = draft.resources.find(resId);
    if (resIt == draft.resources.end() || !resIt->second.operations.contains(opName)) {
        error = QStringLiteral("Operation not found: %1").arg(QString::fromStdString(id.value));
        return false;
    }
    resIt->second.operations[opName] = updated;

    // Validate before writing: a chain edit can introduce a cycle or undefined reference, which
    // must never land an unloadable project.
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

bool ProjectModel::saveProjectDefaultAuth(const std::optional<engine::InlineAuth>& defaultAuth,
                                          QString& error) {
    if (!project_) {
        error = QStringLiteral("No project loaded.");
        return false;
    }
    engine::Project draft = *project_;
    draft.defaultAuth = defaultAuth;
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

    // Validate the whole draft once: only a full-graph check sees cross-operation cycles or
    // undefined references.
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

    engine::Operation moved = resIt->second.operations.at(opName);
    moved.id = engine::OperationId{resId.value + "." + trimmed};
    resIt->second.operations.erase(opName);
    resIt->second.operations[trimmed] = std::move(moved);
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

    // Move the resource under its new id, re-qualifying every operation, then re-point
    // cross-resource depends_on in one pass.
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
    // Publish first so the UI reflects the rename, then surface a failure to remove the orphaned
    // old file.
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
    // Gather the resource's operation ids so cross-resource depends_on entries into it can be
    // dropped.
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
    // Publish first, then surface a failure to remove the orphaned file.
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

    // Route every mutating path through the same validation gate, even though an empty resource
    // can't break the graph.
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
        // Move the per-env transport config and project default onto the new name so neither
        // dangles.
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
    engine::Operation operation;
    operation.actor = actor;
    operation.method = method;
    operation.pathTemplate = pathTemplate.trimmed().toStdString();
    operation.explicitDependencies = dependencies;
    operation.extractions = extractions;
    return createOperation(resourceId, name, operation, error);
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

    const engine::OperationId newId{resourceId.value + "." + trimmed};
    engine::Operation complete = operation;
    complete.id = newId;
    complete.resource = resourceId;
    resIt->second.operations[trimmed] = std::move(complete);

    if (auto valid = engine::validateProject(draft); !valid) {
        error = QString::fromStdString(valid.error().detail);
        return std::nullopt;
    }
    auto written = engine::writeProject(root_, draft, /*overwrite=*/true);
    if (!written) {
        error = QString::fromStdString(written.error().detail);
        return std::nullopt;
    }
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
