// ProjectModel — see header. Wraps engine::parseProject and exposes the
// validated Project to the views.
#include "ProjectModel.h"

#include <chainapi/engine/Factories.h>

#include <utility>

namespace chainapi::desktop {

ProjectModel::ProjectModel(QObject* parent) : QObject(parent) {}

ProjectModel::~ProjectModel() = default;

void ProjectModel::loadFromDirectory(const QString& directory) {
    const std::filesystem::path dir{directory.toStdString()};
    const auto yaml = dir / "chainapi.yaml";

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

}  // namespace chainapi::desktop
