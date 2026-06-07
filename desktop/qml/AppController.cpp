// AppController — see header.
#include "AppController.h"

#include "../src/application/ProjectModel.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>

#include <utility>
#include <vector>

namespace reqloom::desktop::qml {

namespace {

/// Locate the bundled sample project so first run is useful without a dialog.
/// Walks up from the executable directory (mirrors the Widgets App).
[[nodiscard]] QString locateSampleProject() {
    QDir dir(QCoreApplication::applicationDirPath());
    for (int hops = 0; hops < 8; ++hops) {
        const QString candidate = dir.filePath(QStringLiteral("samples/marketplace/reqloom.yaml"));
        if (QFileInfo::exists(candidate)) {
            return dir.filePath(QStringLiteral("samples/marketplace"));
        }
        if (!dir.cdUp()) {
            break;
        }
    }
    return {};
}

[[nodiscard]] QString methodLabel(engine::HttpMethod method) {
    switch (method) {
        case engine::HttpMethod::Get:
            return QStringLiteral("GET");
        case engine::HttpMethod::Post:
            return QStringLiteral("POST");
        case engine::HttpMethod::Put:
            return QStringLiteral("PUT");
        case engine::HttpMethod::Patch:
            return QStringLiteral("PATCH");
        case engine::HttpMethod::Delete:
            return QStringLiteral("DELETE");
        case engine::HttpMethod::Head:
            return QStringLiteral("HEAD");
        case engine::HttpMethod::Options:
            return QStringLiteral("OPTIONS");
    }
    return QStringLiteral("GET");
}

[[nodiscard]] engine::HttpMethod methodFromLabel(const QString& label) {
    const QString upper = label.trimmed().toUpper();
    if (upper == QLatin1String("POST")) {
        return engine::HttpMethod::Post;
    }
    if (upper == QLatin1String("PUT")) {
        return engine::HttpMethod::Put;
    }
    if (upper == QLatin1String("PATCH")) {
        return engine::HttpMethod::Patch;
    }
    if (upper == QLatin1String("DELETE")) {
        return engine::HttpMethod::Delete;
    }
    if (upper == QLatin1String("HEAD")) {
        return engine::HttpMethod::Head;
    }
    if (upper == QLatin1String("OPTIONS")) {
        return engine::HttpMethod::Options;
    }
    return engine::HttpMethod::Get;
}

/// Derive the extraction source from the path prefix, mirroring the engine's
/// YamlSchemaParser (and the old ExtractionTableEditor) so the in-memory
/// Extraction matches what a reload would produce.
[[nodiscard]] engine::Extraction::Source sourceForPath(const std::string& path) {
    if (path.starts_with("$.headers.")) {
        return engine::Extraction::Source::Header;
    }
    if (path.starts_with("$.cookies.")) {
        return engine::Extraction::Source::Cookie;
    }
    if (path == "$.status_code") {
        return engine::Extraction::Source::StatusCode;
    }
    return engine::Extraction::Source::JsonPath;
}

/// Mirror of ProjectModel's id-breaking guard for live dialog validation.
[[nodiscard]] bool hasIdBreakingChars(const QString& name) {
    return name.contains(QLatin1Char('.')) || name.contains(QLatin1Char('/')) ||
           name.contains(QLatin1Char('\\'));
}

}  // namespace

AppController::AppController(QObject* parent)
    : QObject(parent),
      project_(std::make_unique<ProjectModel>()),
      bootstrapper_(std::make_unique<Bootstrapper>()),
      runController_(std::make_unique<RunController>(bootstrapper_->engine(), *project_, this)) {
    connect(project_.get(), &ProjectModel::loaded, this, &AppController::onLoaded);
    connect(project_.get(), &ProjectModel::saved, this, &AppController::onLoaded);
    connect(project_.get(), &ProjectModel::loadFailed, this, &AppController::onLoadFailed);

    connect(runController_.get(), &RunController::runningChanged, this, [this](bool running) {
        running_ = running;
        emit runningChanged();
    });
    connect(runController_.get(),
            &RunController::responseReceived,
            this,
            [this](int,
                   int status,
                   const QString& headers,
                   int bodySize,
                   qint64 elapsedMs,
                   const QString& body) {
                hasResponse_ = true;
                respStatus_ = status;
                respHeaders_ = headers;
                respBodySize_ = bodySize;
                respElapsedMs_ = static_cast<int>(elapsedMs);
                respBody_ = body;
                emit responseChanged();
            });
    connect(runController_.get(), &RunController::runEnded, this, [this](const QString& outcome) {
        runOutcome_ = outcome;
        emit responseChanged();
    });

    treeFilter_.setSourceModel(&tree_);

    loadSampleIfPresent();
}

AppController::~AppController() = default;

int AppController::resourceCount() const {
    return project_->hasProject() ? static_cast<int>(project_->project().resources.size()) : 0;
}

void AppController::openProject(const QUrl& directory) {
    const QString path = directory.isLocalFile() ? directory.toLocalFile() : directory.toString();
    project_->loadFromDirectory(path);
}

void AppController::onLoaded() {
    projectName_ = project_->name();
    resources_.reload(project_->project());
    tree_.populate(project_->project());
    status_ = QStringLiteral("%1 modules").arg(resourceCount());

    environments_ = project_->environmentNames();
    if (environment_.isEmpty() || !environments_.contains(environment_)) {
        environment_ = project_->defaultEnvironment();
        if (environment_.isEmpty() && !environments_.isEmpty()) {
            environment_ = environments_.front();
        }
        emit environmentChanged();
    }

    // Auto-select the first module so the center pane isn't empty on open.
    selectedModule_.clear();
    operations_.reset();
    if (project_->hasProject() && !project_->project().resources.empty()) {
        selectModule(QString::fromStdString(project_->project().resources.begin()->first.value));
    }
    emit projectChanged();
}

void AppController::selectModule(const QString& moduleName) {
    if (!project_->hasProject()) {
        return;
    }
    const auto& resources = project_->project().resources;
    const auto it = resources.find(engine::ResourceId{moduleName.toStdString()});
    if (it == resources.end()) {
        return;
    }
    selectedModule_ = moduleName;
    operations_.reload(it->second);
    closeOperation();
    emit selectionChanged();
}

void AppController::selectOperation(const QString& moduleName, const QString& opName) {
    if (!project_->hasProject()) {
        return;
    }
    const auto& resources = project_->project().resources;
    const auto resIt = resources.find(engine::ResourceId{moduleName.toStdString()});
    if (resIt == resources.end()) {
        return;
    }
    const auto opIt = resIt->second.operations.find(opName.toStdString());
    if (opIt == resIt->second.operations.end()) {
        return;
    }
    const engine::Operation& op = opIt->second;

    opName_ = opName;
    opMethod_ = methodLabel(op.method);
    opPath_ = QString::fromStdString(op.pathTemplate);
    opActor_ = QString::fromStdString(op.actor.value);

    if (op.bodyTemplate) {
        opBody_ = QString::fromStdString(*op.bodyTemplate);
    } else if (op.bodyForm) {
        QString form;
        for (const auto& [key, value] : *op.bodyForm) {
            form += QString::fromStdString(key) + " = " + QString::fromStdString(value) + '\n';
        }
        opBody_ = form.trimmed();
    } else {
        opBody_.clear();
    }

    opHeaders_.reload(op.headers);
    opQuery_.reload(op.queryParams);

    std::vector<std::pair<QString, QString>> extractRows;
    extractRows.reserve(op.extractions.size());
    for (const auto& ext : op.extractions) {
        extractRows.emplace_back(QString::fromStdString(ext.variableName),
                                 QString::fromStdString(ext.sourcePath));
    }
    opExtractions_.reloadPairs(std::move(extractRows));

    opDependencies_.clear();
    for (const auto& dep : op.explicitDependencies) {
        opDependencies_.append(QString::fromStdString(dep.value));
    }

    hasOperation_ = true;
    emit operationChanged();
}

void AppController::closeOperation() {
    if (!hasOperation_ && opName_.isEmpty()) {
        return;
    }
    hasOperation_ = false;
    opName_.clear();
    opMethod_.clear();
    opPath_.clear();
    opActor_.clear();
    opBody_.clear();
    opDependencies_.clear();
    opHeaders_.reset();
    opQuery_.reset();
    opExtractions_.reset();
    emit operationChanged();
}

void AppController::setEnvironment(const QString& env) {
    if (env == environment_) {
        return;
    }
    environment_ = env;
    emit environmentChanged();
}

void AppController::setCaptureBodies(bool capture) {
    if (capture == captureBodies_) {
        return;
    }
    captureBodies_ = capture;
    runController_->setCaptureResponseBodies(capture);
    emit captureBodiesChanged();
}

void AppController::runSelected(bool clean, bool dryRun) {
    if (!hasOperation_ || running_) {
        return;
    }
    const QString target = selectedModule_ + '.' + opName_;
    runController_->run(target, environment_, clean, dryRun);
}

void AppController::onLoadFailed(const QString& code, const QString& detail) {
    projectName_.clear();
    resources_.reset();
    operations_.reset();
    tree_.clear();
    selectedModule_.clear();
    status_ = QStringLiteral("Load failed (%1): %2").arg(code, detail);
    emit projectChanged();
    emit selectionChanged();
}

void AppController::loadSampleIfPresent() {
    if (const QString sample = locateSampleProject(); !sample.isEmpty()) {
        project_->loadFromDirectory(sample);
    }
}

int AppController::operationCount() const {
    if (!project_->hasProject()) {
        return 0;
    }
    int count = 0;
    for (const auto& [resId, resource] : project_->project().resources) {
        count += static_cast<int>(resource.operations.size());
    }
    return count;
}

int AppController::actorCount() const {
    return project_->hasProject() ? static_cast<int>(project_->project().actors.size()) : 0;
}

QStringList AppController::moduleNames() const {
    QStringList names;
    if (project_->hasProject()) {
        for (const auto& [resId, resource] : project_->project().resources) {
            names.append(QString::fromStdString(resId.value));
        }
    }
    return names;
}

QStringList AppController::actorNames() const {
    QStringList names;
    if (project_->hasProject()) {
        for (const auto& [actorId, actor] : project_->project().actors) {
            names.append(QString::fromStdString(actorId.value));
        }
    }
    return names;
}

QStringList AppController::operationIds() const {
    QStringList ids;
    if (project_->hasProject()) {
        for (const auto& [resId, resource] : project_->project().resources) {
            for (const auto& [opName, op] : resource.operations) {
                ids.append(QString::fromStdString(op.id.value));
            }
        }
    }
    return ids;
}

void AppController::selectOperationById(const QString& operationId) {
    const qsizetype dot = operationId.indexOf(QLatin1Char('.'));
    if (dot < 0) {
        return;
    }
    const QString module = operationId.left(dot);
    const QString opName = operationId.mid(dot + 1);
    if (selectedModule_ != module) {
        selectModule(module);
    }
    selectOperation(module, opName);
}

void AppController::activateOperationById(const QString& operationId) {
    selectOperationById(operationId);
    if (hasOperation_) {
        runSelected(/*clean=*/false, /*dryRun=*/false);
    }
}

void AppController::setExplorerFilter(const QString& text) {
    treeFilter_.setFilterText(text);
}

bool AppController::isValidName(const QString& name) const {
    const QString trimmed = name.trimmed();
    return !trimmed.isEmpty() && !hasIdBreakingChars(trimmed);
}

void AppController::createResource(const QString& name, const QString& description) {
    QString error;
    if (project_->createResource(name, description, error)) {
        emit notify(QStringLiteral("Created module “%1”").arg(name.trimmed()), false);
    } else {
        emit notify(error, true);
    }
}

void AppController::prepareNewEndpoint(const QString& /*preselectedResource*/) {
    // Dependency candidates are every existing operation; the dialog filters
    // out self-reference by construction (the new op isn't created yet).
    newEndpointDeps_.setCandidates(operationIds());
    newEndpointExtractions_.clearRows();
}

void AppController::createOperation(const QString& module,
                                    const QString& name,
                                    const QString& method,
                                    const QString& path,
                                    const QString& actor) {
    std::vector<engine::OperationId> dependencies;
    for (const auto& dep : newEndpointDeps_.dependencies()) {
        dependencies.push_back(engine::OperationId{dep});
    }
    std::vector<engine::Extraction> extractions;
    for (const auto& [variable, sourcePath] : newEndpointExtractions_.pairs()) {
        if (sourcePath.isEmpty()) {
            continue;
        }
        engine::Extraction ext;
        ext.variableName = variable.toStdString();
        ext.sourcePath = sourcePath.toStdString();
        ext.source = sourceForPath(ext.sourcePath);
        extractions.push_back(std::move(ext));
    }

    QString error;
    const auto created = project_->createOperation(engine::ResourceId{module.toStdString()},
                                                   name,
                                                   methodFromLabel(method),
                                                   path,
                                                   engine::ActorId{actor.toStdString()},
                                                   dependencies,
                                                   extractions,
                                                   error);
    if (created) {
        emit notify(
            QStringLiteral("Created endpoint “%1”").arg(QString::fromStdString(created->value)),
            false);
        selectOperationById(QString::fromStdString(created->value));
    } else {
        emit notify(error, true);
    }
}

void AppController::renameOperation(const QString& operationId, const QString& newName) {
    QString error;
    if (project_->renameOperation(engine::OperationId{operationId.toStdString()}, newName, error)) {
        emit notify(QStringLiteral("Renamed endpoint"), false);
    } else {
        emit notify(error, true);
    }
}

void AppController::deleteOperation(const QString& operationId) {
    QString error;
    if (project_->deleteOperation(engine::OperationId{operationId.toStdString()}, error)) {
        emit notify(QStringLiteral("Deleted endpoint"), false);
    } else {
        emit notify(error, true);
    }
}

void AppController::renameResource(const QString& resourceId, const QString& newName) {
    QString error;
    if (project_->renameResource(engine::ResourceId{resourceId.toStdString()}, newName, error)) {
        emit notify(QStringLiteral("Renamed module"), false);
    } else {
        emit notify(error, true);
    }
}

void AppController::deleteResource(const QString& resourceId) {
    QString error;
    if (project_->deleteResource(engine::ResourceId{resourceId.toStdString()}, error)) {
        emit notify(QStringLiteral("Deleted module"), false);
    } else {
        emit notify(error, true);
    }
}

void AppController::selectExample(const QString& operationId, const QString& /*exampleName*/) {
    // Load the operation; showing the stored example response is wired in WS-C
    // once SavedResponseStore is bridged.
    selectOperationById(operationId);
    // TODO(WS-C): show the saved example's stored response in the response panel.
}

void AppController::renameExample(const QString& /*operationId*/, const QString& /*exampleName*/) {
    // TODO(WS-C): wire to SavedResponseStore::rename once the examples bridge lands.
    emit notify(QStringLiteral("Saved examples are available after WS-C"), true);
}

void AppController::duplicateExample(const QString& /*operationId*/,
                                     const QString& /*exampleName*/) {
    // TODO(WS-C): wire to SavedResponseStore::duplicate once the examples bridge lands.
    emit notify(QStringLiteral("Saved examples are available after WS-C"), true);
}

void AppController::deleteExample(const QString& /*operationId*/, const QString& /*exampleName*/) {
    // TODO(WS-C): wire to SavedResponseStore::remove once the examples bridge lands.
    emit notify(QStringLiteral("Saved examples are available after WS-C"), true);
}

}  // namespace reqloom::desktop::qml
