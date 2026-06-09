// AppController — see header.
#include "AppController.h"

#include "../../src/application/EnvironmentSettings.h"
#include "../../src/application/ProjectModel.h"
#include "../../src/widgets/LineDiff.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

#include <map>
#include <string>
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

/// Convert an ordered string map into QString pairs for an EditableKeyValueModel
/// (insertion/sort order preserved). Used to seed the edit controls from an op.
[[nodiscard]] std::vector<std::pair<QString, QString>> toEditPairs(
    const std::map<std::string, std::string>& pairs) {
    std::vector<std::pair<QString, QString>> out;
    out.reserve(pairs.size());
    for (const auto& [key, value] : pairs) {
        out.emplace_back(QString::fromStdString(key), QString::fromStdString(value));
    }
    return out;
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
                shownExample_.clear();
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

    // Bridge ALL streamed RunController signals into the timeline model so the
    // panel mirrors the old Widgets TimelinePanel (steps, requests, responses,
    // extractions, skips, failures). Receiver-bound, function-pointer form.
    connect(
        runController_.get(), &RunController::runStarted, &timeline_, &TimelineModel::onRunStarted);
    connect(runController_.get(),
            &RunController::stepStarted,
            &timeline_,
            &TimelineModel::onStepStarted);
    connect(runController_.get(),
            &RunController::stepSkipped,
            &timeline_,
            &TimelineModel::onStepSkipped);
    connect(runController_.get(),
            &RunController::requestPrepared,
            &timeline_,
            &TimelineModel::onRequestPrepared);
    connect(runController_.get(),
            &RunController::responseReceived,
            &timeline_,
            &TimelineModel::onResponseReceived);
    connect(runController_.get(),
            &RunController::extractionCompleted,
            &timeline_,
            &TimelineModel::onExtractionCompleted);
    connect(
        runController_.get(), &RunController::stepFailed, &timeline_, &TimelineModel::onStepFailed);
    connect(runController_.get(), &RunController::runEnded, &timeline_, &TimelineModel::onRunEnded);

    treeFilter_.setSourceModel(&tree_);

    // Any change to an edit model re-evaluates the live tab counts + banner,
    // and the dependency model also re-renders the chain preview. Receiver-
    // bound, function-pointer form (auto-disconnect on destruction).
    const auto onEditModelChanged = [this]() {
        emit editChanged();
    };
    for (QAbstractItemModel* model : {static_cast<QAbstractItemModel*>(&editHeaders_),
                                      static_cast<QAbstractItemModel*>(&editQuery_),
                                      static_cast<QAbstractItemModel*>(&editForm_),
                                      static_cast<QAbstractItemModel*>(&editExtractions_)}) {
        connect(model, &QAbstractItemModel::dataChanged, this, onEditModelChanged);
        connect(model, &QAbstractItemModel::rowsInserted, this, onEditModelChanged);
        connect(model, &QAbstractItemModel::rowsRemoved, this, onEditModelChanged);
        connect(model, &QAbstractItemModel::modelReset, this, onEditModelChanged);
    }
    const auto onDepsChanged = [this]() {
        emit editChanged();
        emit chainChanged();
    };
    connect(&editDependencies_, &QAbstractItemModel::dataChanged, this, onDepsChanged);
    connect(&editDependencies_, &QAbstractItemModel::rowsInserted, this, onDepsChanged);
    connect(&editDependencies_, &QAbstractItemModel::rowsRemoved, this, onDepsChanged);
    connect(&editDependencies_, &QAbstractItemModel::modelReset, this, onDepsChanged);

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

    // Point the saved-example store at this project (per-project isolation) and
    // re-apply the explorer's example child rows from disk.
    exampleStore_.setProjectRoot(project_->rootPath());

    environments_ = project_->environmentNames();
    if (environment_.isEmpty() || !environments_.contains(environment_)) {
        // Restore the per-project saved environment first, then fall back to
        // the project default (parity gap T-D9 from the QML Migration Roadmap).
        QSettings settings;
        const QString saved = EnvironmentSettings::load(settings, project_->rootPath());
        if (!saved.isEmpty() && environments_.contains(saved)) {
            environment_ = saved;
        } else {
            environment_ = project_->defaultEnvironment();
            if (environment_.isEmpty() && !environments_.isEmpty()) {
                environment_ = environments_.front();
            }
        }
        emit environmentChanged();
    }

    // Auto-select the first module so the center pane isn't empty on open.
    selectedModule_.clear();
    operations_.reset();
    if (project_->hasProject() && !project_->project().resources.empty()) {
        selectModule(QString::fromStdString(project_->project().resources.begin()->first.value));
    }
    refreshExamples();
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
    if (editing_) {
        editing_ = false;
        emit editingChanged();
    }
    chainFieldsLoaded_ = false;
    emit operationChanged();
    emit chainChanged();
    refreshOpenOpExamples();
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
    exampleList_.clear();
    if (editing_) {
        editing_ = false;
        emit editingChanged();
    }
    chainFieldsLoaded_ = false;
    emit operationChanged();
    emit chainChanged();
}

void AppController::setEnvironment(const QString& env) {
    if (env == environment_) {
        return;
    }
    environment_ = env;
    // Persist the selection for this project so it's restored on next load.
    if (!project_->rootPath().isEmpty()) {
        QSettings settings;
        EnvironmentSettings::save(settings, project_->rootPath(), env);
    }
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

void AppController::setEditMethod(const QString& method) {
    if (method == editMethod_) {
        return;
    }
    editMethod_ = method;
    emit editChanged();
}

void AppController::setEditPath(const QString& path) {
    if (path == editPath_) {
        return;
    }
    editPath_ = path;
    emit editChanged();
}

void AppController::setEditActor(const QString& actor) {
    if (actor == editActor_) {
        return;
    }
    editActor_ = actor;
    emit editChanged();
}

void AppController::setEditExpectStatus(const QString& expectStatus) {
    if (expectStatus == editExpectStatus_) {
        return;
    }
    editExpectStatus_ = expectStatus;
    emit editChanged();
}

void AppController::setEditTimeout(int timeoutMs) {
    if (timeoutMs == editTimeout_) {
        return;
    }
    editTimeout_ = timeoutMs;
    emit editChanged();
}

void AppController::setEditForce(bool force) {
    if (force == editForce_) {
        return;
    }
    editForce_ = force;
    emit editChanged();
}

void AppController::setEditBody(const QString& body) {
    if (body == editBody_) {
        return;
    }
    editBody_ = body;
    emit editChanged();
}

void AppController::setEditBodyIsForm(bool isForm) {
    if (isForm == editBodyIsForm_) {
        return;
    }
    editBodyIsForm_ = isForm;
    emit editChanged();
}

QStringList AppController::editDependencyCandidates() const {
    // Every operation except the open one (no self-dependency).
    const QString self = currentOperationId();
    QStringList out;
    for (const QString& id : operationIds()) {
        if (id != self) {
            out.append(id);
        }
    }
    return out;
}

int AppController::editParamsCount() const {
    return static_cast<int>(editQuery_.pairs().size());
}

int AppController::editHeadersCount() const {
    return static_cast<int>(editHeaders_.pairs().size());
}

bool AppController::editBodyFilled() const {
    return editBodyIsForm_ ? !editForm_.pairs().empty() : !editBody_.trimmed().isEmpty();
}

int AppController::editChainCount() const {
    return static_cast<int>(editDependencies_.dependencies().size() +
                            editExtractions_.pairs().size());
}

QVariantList AppController::chainNodes() const {
    // Static view of the operation's declared dependencies in declared order,
    // then the target itself last (mirrors the old RequestEditorPanel chain
    // preview). In Edit mode the deps come from the live picker so the preview
    // updates as the user wires the chain. Implicit ({{var}}) deps and the
    // full topological order are resolved by the engine after a Dry Run.
    QVariantList nodes;
    if (!project_->hasProject() || !hasOperation_) {
        return nodes;
    }
    const QString opId = currentOperationId();
    const auto* op = project_->findOperation(engine::OperationId{opId.toStdString()});
    if (op == nullptr) {
        return nodes;
    }

    std::vector<QString> deps;
    if (editing_ && chainFieldsLoaded_) {
        for (const auto& dep : editDependencies_.dependencies()) {
            deps.emplace_back(QString::fromStdString(dep));
        }
    } else {
        for (const auto& dep : op->explicitDependencies) {
            deps.emplace_back(QString::fromStdString(dep.value));
        }
    }
    if (deps.empty()) {
        return nodes;
    }

    const auto nodeFor = [this](const QString& id, bool isTarget) {
        QVariantMap node;
        node.insert(QStringLiteral("operationId"), id);
        const auto* depOp = project_->findOperation(engine::OperationId{id.toStdString()});
        node.insert(QStringLiteral("method"),
                    depOp != nullptr ? methodLabel(depOp->method) : QString{});
        node.insert(QStringLiteral("isTarget"), isTarget);
        return node;
    };
    for (const QString& dep : deps) {
        nodes.append(nodeFor(dep, /*isTarget=*/false));
    }
    nodes.append(nodeFor(opId, /*isTarget=*/true));
    return nodes;
}

void AppController::beginEdit() {
    if (!hasOperation_ || !project_->hasProject()) {
        return;
    }
    const auto* op =
        project_->findOperation(engine::OperationId{currentOperationId().toStdString()});
    if (op == nullptr) {
        return;
    }

    // Seed every editable control from the operation so a fresh edit starts as
    // a faithful copy the user then tweaks.
    editMethod_ = methodLabel(op->method);
    editPath_ = QString::fromStdString(op->pathTemplate);
    editActor_ = QString::fromStdString(op->actor.value);

    if (!op->expectStatusList.empty()) {
        QStringList codes;
        for (const int code : op->expectStatusList) {
            codes.append(QString::number(code));
        }
        editExpectStatus_ = codes.join(QLatin1Char(','));
    } else if (op->expectStatus) {
        editExpectStatus_ = QString::number(*op->expectStatus);
    } else {
        editExpectStatus_.clear();
    }

    editTimeout_ = op->timeout ? static_cast<int>(op->timeout->count()) : 0;
    editForce_ = op->force;

    editHeaders_.setPairs(toEditPairs(op->headers));
    editQuery_.setPairs(toEditPairs(op->queryParams));

    if (op->bodyForm) {
        editBodyIsForm_ = true;
        editForm_.setPairs(toEditPairs(*op->bodyForm));
        editBody_.clear();
    } else {
        editBodyIsForm_ = false;
        editBody_ = op->bodyTemplate ? QString::fromStdString(*op->bodyTemplate) : QString{};
        editForm_.clearRows();
    }

    editDependencies_.setCandidates(editDependencyCandidates());
    std::vector<std::string> deps;
    deps.reserve(op->explicitDependencies.size());
    for (const auto& dep : op->explicitDependencies) {
        deps.push_back(dep.value);
    }
    editDependencies_.setDependencies(deps);

    std::vector<std::pair<QString, QString>> extractRows;
    extractRows.reserve(op->extractions.size());
    for (const auto& ext : op->extractions) {
        extractRows.emplace_back(QString::fromStdString(ext.variableName),
                                 QString::fromStdString(ext.sourcePath));
    }
    editExtractions_.setPairs(std::move(extractRows));

    chainFieldsLoaded_ = true;
    editing_ = true;
    emit editingChanged();
    emit editChanged();
    emit chainChanged();
}

void AppController::cancelEdit() {
    if (!editing_) {
        return;
    }
    editing_ = false;
    emit editingChanged();
    emit chainChanged();
}

RequestOverride AppController::buildOverride() const {
    RequestOverride ov;
    ov.active = true;
    ov.method = editMethod_;
    ov.path = editPath_;
    for (const auto& [key, value] : editHeaders_.pairs()) {
        ov.headers.insert_or_assign(key.toStdString(), value.toStdString());
    }
    for (const auto& [key, value] : editQuery_.pairs()) {
        ov.queryParams.insert_or_assign(key.toStdString(), value.toStdString());
    }
    ov.actor = editActor_;
    ov.expectStatus = editExpectStatus_;
    ov.timeoutMs = editTimeout_;
    ov.forceReRun = editForce_;

    ov.bodyIsForm = editBodyIsForm_;
    if (editBodyIsForm_) {
        for (const auto& [key, value] : editForm_.pairs()) {
            ov.formFields.insert_or_assign(key.toStdString(), value.toStdString());
        }
    } else {
        ov.body = editBody_;
    }

    // Chain edits are opt-in: only stamped when the Chain tab was seeded for
    // this op, so a request-only override never wipes its depends_on / extract.
    ov.chainEdited = chainFieldsLoaded_;
    ov.dependencies = editDependencies_.dependencies();
    for (const auto& [variable, sourcePath] : editExtractions_.pairs()) {
        if (sourcePath.isEmpty()) {
            continue;
        }
        engine::Extraction ext;
        ext.variableName = variable.toStdString();
        ext.sourcePath = sourcePath.toStdString();
        ext.source = sourceForPath(ext.sourcePath);
        ov.extractions.push_back(std::move(ext));
    }
    return ov;
}

void AppController::applyAndRun(bool clean, bool dryRun) {
    if (!hasOperation_ || running_) {
        return;
    }
    const QString target = selectedModule_ + '.' + opName_;
    runController_->runWithOverride(target, environment_, clean, dryRun, buildOverride());
}

void AppController::saveOperation() {
    if (!hasOperation_ || !project_->hasProject()) {
        return;
    }
    const QString opId = currentOperationId();
    const engine::OperationId id{opId.toStdString()};
    const auto* op = project_->findOperation(id);
    if (op == nullptr) {
        emit notify(QStringLiteral("No operation to save"), true);
        return;
    }

    // Patch a copy of the real operation via the shared override path, then
    // persist. saveOperation validates through the engine, so a chain that
    // forms a cycle (or names an undefined op) is rejected and disk is left
    // untouched.
    engine::Operation updated = *op;
    applyOverrideToOperation(updated, buildOverride());

    QString error;
    if (project_->saveOperation(id, updated, error)) {
        // saveOperation emits `saved` → onLoaded reset the selection; reopen
        // the operation and leave Edit mode so the read preview reflects disk.
        editing_ = false;
        emit editingChanged();
        selectOperationById(opId);
        emit notify(QStringLiteral("Saved “%1” to project").arg(opId), false);
    } else {
        emit notify(error, true);
    }
}

void AppController::cancelRun() {
    runController_->cancelRun();
}

void AppController::resetCaches() {
    if (running_) {
        emit notify(QStringLiteral("Can't reset caches while a run is in flight"), true);
        return;
    }
    runController_->resetCaches();
    emit notify(QStringLiteral("Session + extraction caches cleared"), false);
}

QString AppController::currentOperationId() const {
    if (!hasOperation_ || selectedModule_.isEmpty() || opName_.isEmpty()) {
        return {};
    }
    return selectedModule_ + '.' + opName_;
}

void AppController::saveResponse(const QString& name) {
    const QString opId = currentOperationId();
    if (opId.isEmpty()) {
        emit notify(QStringLiteral("Open an operation before saving an example"), true);
        return;
    }
    if (!hasResponse_) {
        emit notify(QStringLiteral("No response to save yet"), true);
        return;
    }
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
        emit notify(QStringLiteral("Example name can't be empty"), true);
        return;
    }
    SavedResponse response;
    response.name = trimmed;
    response.status = respStatus_;
    response.headers = respHeaders_;
    response.body = respBody_;
    response.elapsedMs = respElapsedMs_;
    if (exampleStore_.save(opId, response)) {
        refreshExamples();
        emit notify(QStringLiteral("Saved example \u201C%1\u201D").arg(trimmed), false);
    } else {
        emit notify(QStringLiteral("No project to save the example into"), true);
    }
}

void AppController::showExample(const QString& name) {
    const QString opId = currentOperationId();
    if (!opId.isEmpty()) {
        selectExample(opId, name);
    }
}

QString AppController::exampleBody(const QString& name) const {
    const QString opId = currentOperationId();
    if (opId.isEmpty()) {
        return {};
    }
    for (const SavedResponse& r : exampleStore_.list(opId)) {
        if (r.name == name) {
            return r.body;
        }
    }
    return {};
}

QVariantList AppController::lineDiff(const QString& oldText, const QString& newText) const {
    QVariantList rows;
    for (const auto& line : widgets::diff::lineDiff(oldText, newText)) {
        using Kind = widgets::diff::DiffLine::Kind;
        QString sign = QStringLiteral(" ");
        if (line.kind == Kind::Added) {
            sign = QStringLiteral("+");
        } else if (line.kind == Kind::Removed) {
            sign = QStringLiteral("-");
        }
        rows.append(
            QVariantMap{{QStringLiteral("sign"), sign}, {QStringLiteral("text"), line.text}});
    }
    return rows;
}

void AppController::copyToClipboard(const QString& text, const QString& label) {
    if (auto* clipboard = QGuiApplication::clipboard(); clipboard != nullptr) {
        clipboard->setText(text);
    }
    const QString what = label.isEmpty() ? QStringLiteral("value") : label;
    emit notify(QStringLiteral("Copied %1").arg(what), false);
}

void AppController::refreshExamples() {
    refreshOpenOpExamples();
    // The explorer's example child rows: opId → ordered example names. This
    // resets the tree model, so only call it on load + after example mutations
    // — never on plain selection (which would collapse the TreeView).
    QMap<QString, QStringList> byOperation;
    for (const QString& id : exampleStore_.operationIds()) {
        QStringList names;
        for (const SavedResponse& r : exampleStore_.list(id)) {
            names.append(r.name);
        }
        byOperation.insert(id, names);
    }
    tree_.setSavedExamples(byOperation);
}

void AppController::refreshOpenOpExamples() {
    const QString opId = currentOperationId();
    if (opId.isEmpty()) {
        exampleList_.clear();
    } else {
        exampleList_.setExamples(exampleStore_.list(opId));
    }
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

void AppController::selectExample(const QString& operationId, const QString& exampleName) {
    // Open the operation, then show the stored example response in the panel.
    selectOperationById(operationId);
    const QList<SavedResponse> saved = exampleStore_.list(operationId);
    for (const SavedResponse& r : saved) {
        if (r.name != exampleName) {
            continue;
        }
        hasResponse_ = true;
        shownExample_ = r.name;
        respStatus_ = r.status;
        respHeaders_ = r.headers;
        respBody_ = r.body;
        respBodySize_ = static_cast<int>(r.body.toUtf8().size());
        respElapsedMs_ = static_cast<int>(r.elapsedMs);
        runOutcome_.clear();
        emit responseChanged();
        return;
    }
    emit notify(QStringLiteral("Saved example not found"), true);
}

void AppController::renameExample(const QString& operationId,
                                  const QString& oldName,
                                  const QString& newName) {
    if (exampleStore_.rename(operationId, oldName, newName.trimmed())) {
        // If the renamed example is the one on screen, keep the label in sync.
        if (shownExample_ == oldName) {
            shownExample_ = newName.trimmed();
            emit responseChanged();
        }
        refreshExamples();
        emit notify(QStringLiteral("Renamed example to \u201C%1\u201D").arg(newName.trimmed()),
                    false);
    } else {
        emit notify(QStringLiteral("Couldn't rename example (empty or duplicate name)"), true);
    }
}

void AppController::duplicateExample(const QString& operationId, const QString& exampleName) {
    const QString created = exampleStore_.duplicate(operationId, exampleName);
    if (created.isEmpty()) {
        emit notify(QStringLiteral("Couldn't duplicate example"), true);
        return;
    }
    refreshExamples();
    emit notify(QStringLiteral("Duplicated as \u201C%1\u201D").arg(created), false);
}

void AppController::deleteExample(const QString& operationId, const QString& exampleName) {
    exampleStore_.remove(operationId, exampleName);
    refreshExamples();
    emit notify(QStringLiteral("Deleted example \u201C%1\u201D").arg(exampleName), false);
}

}  // namespace reqloom::desktop::qml
