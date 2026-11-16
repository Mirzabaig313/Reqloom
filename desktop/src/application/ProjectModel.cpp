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
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace reqloom::desktop {

namespace {

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
