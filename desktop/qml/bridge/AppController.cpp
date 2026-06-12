// AppController — see header.
#include "AppController.h"

#include "../../src/application/EnvironmentSettings.h"
#include "../../src/application/ProjectModel.h"
#include "../../src/views/HookEditorDialog.h"
#include "../../src/widgets/LineDiff.h"
#include "ThemeController.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QStringList>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QDialog>

#include <algorithm>
#include <filesystem>
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

    // Capture response bodies by default so the timeline shows full request /
    // response detail (including error bodies) without an opt-in toggle. The
    // RunController defaults to off, so push our default through explicitly.
    runController_->setCaptureResponseBodies(captureBodies_);

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

void AppController::setEditBodyType(const QString& type) {
    if (type == editBodyType_) {
        return;
    }
    editBodyType_ = type;
    editBodyIsForm_ =
        (type == QStringLiteral("form-data") || type == QStringLiteral("x-www-form-urlencoded"));

    QString desired;
    if (type == QStringLiteral("json") || type == QStringLiteral("graphql")) {
        desired = QStringLiteral("application/json");
    } else if (type == QStringLiteral("xml")) {
        desired = QStringLiteral("application/xml");
    } else if (type == QStringLiteral("text")) {
        desired = QStringLiteral("text/plain");
    } else if (type == QStringLiteral("form-data")) {
        desired = QStringLiteral("multipart/form-data");
    } else if (type == QStringLiteral("x-www-form-urlencoded")) {
        desired = QStringLiteral("application/x-www-form-urlencoded");
    }
    if (!desired.isEmpty()) {
        setManagedContentType(desired);
    }
    emit editChanged();
}

void AppController::setManagedContentType(const QString& desired) {
    static const QStringList kManaged = {
        QStringLiteral("application/json"),
        QStringLiteral("application/xml"),
        QStringLiteral("text/plain"),
        QStringLiteral("multipart/form-data"),
        QStringLiteral("application/x-www-form-urlencoded"),
    };
    auto pairs = editHeaders_.pairs();
    QString current;
    for (const auto& [k, v] : pairs) {
        if (k.compare(QStringLiteral("Content-Type"), Qt::CaseInsensitive) == 0) {
            current = v;
            break;
        }
    }
    // Preserve a custom Content-Type — only overwrite empty or canonical ones.
    const QString base = current.section(';', 0, 0).trimmed();
    const bool isManaged =
        current.isEmpty() || std::ranges::any_of(kManaged, [&](const QString& m) {
            return base.compare(m, Qt::CaseInsensitive) == 0;
        });
    if (!isManaged) {
        return;
    }
    bool found = false;
    for (auto& [k, v] : pairs) {
        if (k.compare(QStringLiteral("Content-Type"), Qt::CaseInsensitive) == 0) {
            v = desired;
            found = true;
            break;
        }
    }
    if (!found) {
        pairs.emplace_back(QStringLiteral("Content-Type"), desired);
    }
    editHeaders_.setPairs(std::move(pairs));
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
    if (editBodyType_ == QStringLiteral("none")) {
        return false;
    }
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

    // Infer the body kind so the selector lands on the right tab. The
    // Content-Type header disambiguates raw kinds (json/xml/text) and multipart
    // vs urlencoded form bodies.
    QString contentType;
    for (const auto& [k, v] : op->headers) {
        if (QString::fromStdString(k).compare(QStringLiteral("Content-Type"),
                                              Qt::CaseInsensitive) == 0) {
            contentType = QString::fromStdString(v);
            break;
        }
    }
    const QString ctypeBase = contentType.section(QLatin1Char(';'), 0, 0).trimmed();

    if (op->bodyForm) {
        editBodyIsForm_ = true;
        editForm_.setPairs(toEditPairs(*op->bodyForm));
        editBody_.clear();
        const bool anyFile = std::ranges::any_of(*op->bodyForm, [](const auto& kv) {
            return QString::fromStdString(kv.second).startsWith(QLatin1Char('@'));
        });
        const bool multipart = anyFile || ctypeBase.compare(QStringLiteral("multipart/form-data"),
                                                            Qt::CaseInsensitive) == 0;
        editBodyType_ =
            multipart ? QStringLiteral("form-data") : QStringLiteral("x-www-form-urlencoded");
    } else if (op->bodyTemplate && !QString::fromStdString(*op->bodyTemplate).trimmed().isEmpty()) {
        editBodyIsForm_ = false;
        editBody_ = QString::fromStdString(*op->bodyTemplate);
        editForm_.clearRows();
        if (ctypeBase.contains(QStringLiteral("xml"), Qt::CaseInsensitive)) {
            editBodyType_ = QStringLiteral("xml");
        } else if (ctypeBase.startsWith(QStringLiteral("text/"), Qt::CaseInsensitive)) {
            editBodyType_ = QStringLiteral("text");
        } else {
            editBodyType_ = QStringLiteral("json");
        }
    } else {
        editBodyIsForm_ = false;
        editBody_.clear();
        editForm_.clearRows();
        editBodyType_ = QStringLiteral("none");
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

void AppController::openHookEditor() {
    const QString opId = currentOperationId();
    if (opId.isEmpty()) {
        emit notify(QStringLiteral("Open an operation before editing hooks"), true);
        return;
    }
    const engine::OperationId id{opId.toStdString()};
    const auto* op = project_->findOperation(id);
    if (op == nullptr) {
        emit notify(QStringLiteral("No operation to edit hooks for"), true);
        return;
    }

    const auto toQ = [](const std::optional<std::string>& s) {
        return s ? QString::fromStdString(*s) : QString{};
    };
    const QString preRef = toQ(op->preRequestScriptRef);
    const QString postRef = toQ(op->postResponseScriptRef);

    const auto appearance = ThemeController::create(nullptr, nullptr)->resolvedAppearance();
    const auto theme = theming::Theme::resolve(appearance);

    HookEditorDialog dialog(opId,
                            toQ(op->preRequestScript),
                            preRef,
                            toQ(op->postResponseScript),
                            postRef,
                            theme.palette());
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    // Persist. A file-referenced hook is written back to its `.js` file (the
    // writer keeps the reference); an inline hook is stored on the operation
    // and saved as YAML. Empty inline content clears the hook.
    const QString root = project_->rootPath();
    const auto applyHook = [&](std::optional<std::string>& script,
                               const QString& edited,
                               const QString& refPath) -> bool {
        if (!refPath.isEmpty()) {
            // Defence-in-depth: resolve the ref under the project root and
            // reject any path that escapes it .
            namespace fs = std::filesystem;
            std::error_code ec;
            const fs::path rootDir = fs::weakly_canonical(fs::path(root.toStdString()), ec);
            const fs::path target =
                fs::weakly_canonical(fs::path(root.toStdString()) / refPath.toStdString(), ec);
            if (ec) {
                return false;
            }
            const fs::path rel = target.lexically_relative(rootDir);
            if (rel.empty() || rel.string().starts_with("..")) {
                return false;  // escapes the project root
            }
            const QString abs = QString::fromStdString(target.string());
            QFileInfo(abs).dir().mkpath(QStringLiteral("."));
            QFile file(abs);
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
                return false;
            }
            const QByteArray bytes = edited.toUtf8();
            const bool ok = file.write(bytes) == bytes.size() && file.flush();
            file.close();
            if (!ok || file.error() != QFileDevice::NoError) {
                return false;
            }
            script = edited.toStdString();
            return true;
        }
        if (edited.trimmed().isEmpty()) {
            script.reset();
        } else {
            script = edited.toStdString();
        }
        return true;
    };

    engine::Operation updated = *op;
    if (!applyHook(updated.preRequestScript, dialog.preScript(), preRef) ||
        !applyHook(updated.postResponseScript, dialog.postScript(), postRef)) {
        emit notify(QStringLiteral("Couldn't write a referenced hook file"), true);
        return;
    }

    QString error;
    if (project_->saveOperation(id, updated, error)) {
        // saveOperation rebinds the project (its `saved` signal resets the
        // selection to the endpoint list), so reopen the operation to keep the
        // user where they were.
        selectOperationById(opId);
        emit notify(QStringLiteral("Saved hooks for “%1”").arg(opId), false);
    } else {
        emit notify(error, true);
    }
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

QString AppController::localFileFromUrl(const QUrl& url) const {
    return url.isLocalFile() ? url.toLocalFile() : url.toString();
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

QString AppController::actorAuthLabel(const QString& actorName) const {
    if (actorName.isEmpty()) {
        return QStringLiteral("No authentication");
    }
    if (!project_->hasProject()) {
        return {};
    }
    const auto& actors = project_->project().actors;
    const auto it = actors.find(engine::ActorId{actorName.toStdString()});
    if (it == actors.end()) {
        return {};
    }
    switch (it->second.strategy) {
        case engine::AuthStrategy::Simple:
            return QStringLiteral("Single-step login");
        case engine::AuthStrategy::Chain:
            return QStringLiteral("Multi-step login chain");
        case engine::AuthStrategy::Basic:
            return QStringLiteral("Basic Auth");
        case engine::AuthStrategy::ApiKey:
            return QStringLiteral("API Key");
        case engine::AuthStrategy::OAuth2ClientCredentials:
            return QStringLiteral("OAuth 2.0 (Client Credentials)");
        case engine::AuthStrategy::OAuth2Password:
            return QStringLiteral("OAuth 2.0 (Password)");
        case engine::AuthStrategy::OAuth1:
            return QStringLiteral("OAuth 1.0 (HMAC-SHA1)");
        case engine::AuthStrategy::AwsSigV4:
            return QStringLiteral("AWS Signature v4");
    }
    return QStringLiteral("Custom");
}

namespace {

[[nodiscard]] engine::AuthStrategy authStrategyFromLabel(const QString& label) {
    if (label == QStringLiteral("Multi-step login chain")) {
        return engine::AuthStrategy::Chain;
    }
    if (label == QStringLiteral("Basic Auth")) {
        return engine::AuthStrategy::Basic;
    }
    if (label == QStringLiteral("API Key")) {
        return engine::AuthStrategy::ApiKey;
    }
    if (label == QStringLiteral("OAuth 2.0 (Client Credentials)")) {
        return engine::AuthStrategy::OAuth2ClientCredentials;
    }
    if (label == QStringLiteral("OAuth 2.0 (Password)")) {
        return engine::AuthStrategy::OAuth2Password;
    }
    if (label == QStringLiteral("OAuth 1.0 (HMAC-SHA1)")) {
        return engine::AuthStrategy::OAuth1;
    }
    if (label == QStringLiteral("AWS Signature v4")) {
        return engine::AuthStrategy::AwsSigV4;
    }
    return engine::AuthStrategy::Simple;
}

[[nodiscard]] engine::HttpMethod httpMethodFromLabel(const QString& method) {
    const QString upper = method.trimmed().toUpper();
    if (upper == QStringLiteral("GET")) {
        return engine::HttpMethod::Get;
    }
    if (upper == QStringLiteral("PUT")) {
        return engine::HttpMethod::Put;
    }
    if (upper == QStringLiteral("PATCH")) {
        return engine::HttpMethod::Patch;
    }
    if (upper == QStringLiteral("DELETE")) {
        return engine::HttpMethod::Delete;
    }
    if (upper == QStringLiteral("HEAD")) {
        return engine::HttpMethod::Head;
    }
    if (upper == QStringLiteral("OPTIONS")) {
        return engine::HttpMethod::Options;
    }
    return engine::HttpMethod::Post;
}

[[nodiscard]] std::vector<engine::Extraction> extractionsFromPairs(
    const std::vector<std::pair<QString, QString>>& pairs) {
    std::vector<engine::Extraction> out;
    for (const auto& [key, value] : pairs) {
        if (key.isEmpty()) {
            continue;
        }
        engine::Extraction ext;
        ext.variableName = key.toStdString();
        ext.sourcePath = value.toStdString();
        ext.source = engine::Extraction::Source::JsonPath;
        out.push_back(std::move(ext));
    }
    return out;
}

}  // namespace

QStringList AppController::actorStrategies() const {
    return {QStringLiteral("Basic Auth"),
            QStringLiteral("API Key"),
            QStringLiteral("OAuth 2.0 (Client Credentials)"),
            QStringLiteral("OAuth 2.0 (Password)"),
            QStringLiteral("OAuth 1.0 (HMAC-SHA1)"),
            QStringLiteral("AWS Signature v4"),
            QStringLiteral("Single-step login"),
            QStringLiteral("Multi-step login chain")};
}

QString AppController::actorDescription(const QString& actorId) const {
    if (!project_->hasProject()) {
        return {};
    }
    const auto& actors = project_->project().actors;
    const auto it = actors.find(engine::ActorId{actorId.toStdString()});
    return it == actors.end() ? QString{} : QString::fromStdString(it->second.description);
}

void AppController::prepareNewActor() {
    actorAuthMethod_ = QStringLiteral("POST");
    actorAuthPath_.clear();
    actorAuthBody_.clear();
    actorAuthExpect_.clear();
    actorHasRefresh_ = false;
    actorRefreshMethod_ = QStringLiteral("POST");
    actorRefreshPath_.clear();
    actorRefreshBody_.clear();
    actorConfig_.clearRows();
    actorAuthExtract_.clearRows();
    actorRefreshExtract_.clearRows();
    emit actorEditChanged();
}

void AppController::prepareEditActor(const QString& actorId) {
    actorAuthMethod_ = QStringLiteral("POST");
    actorAuthPath_.clear();
    actorAuthBody_.clear();
    actorAuthExpect_.clear();
    actorHasRefresh_ = false;
    actorRefreshMethod_ = QStringLiteral("POST");
    actorRefreshPath_.clear();
    actorRefreshBody_.clear();

    std::vector<std::pair<QString, QString>> config;
    std::vector<std::pair<QString, QString>> authEx;
    std::vector<std::pair<QString, QString>> refreshEx;

    if (project_->hasProject()) {
        const auto& actors = project_->project().actors;
        const auto it = actors.find(engine::ActorId{actorId.toStdString()});
        if (it != actors.end()) {
            const engine::Actor& actor = it->second;
            for (const auto& [key, value] : actor.authConfig) {
                config.emplace_back(QString::fromStdString(key), QString::fromStdString(value));
            }
            if (!actor.authSteps.empty()) {
                const engine::AuthStep& step = actor.authSteps.front();
                actorAuthMethod_ = methodLabel(step.method);
                actorAuthPath_ = QString::fromStdString(step.pathTemplate);
                actorAuthBody_ =
                    step.bodyTemplate ? QString::fromStdString(*step.bodyTemplate) : QString{};
                actorAuthExpect_ =
                    step.expectStatus ? QString::number(*step.expectStatus) : QString{};
                for (const auto& ext : step.extractions) {
                    authEx.emplace_back(QString::fromStdString(ext.variableName),
                                        QString::fromStdString(ext.sourcePath));
                }
            }
            if (actor.refresh) {
                actorHasRefresh_ = true;
                actorRefreshMethod_ = methodLabel(actor.refresh->method);
                actorRefreshPath_ = QString::fromStdString(actor.refresh->pathTemplate);
                actorRefreshBody_ = actor.refresh->bodyTemplate
                                        ? QString::fromStdString(*actor.refresh->bodyTemplate)
                                        : QString{};
                for (const auto& ext : actor.refresh->extractions) {
                    refreshEx.emplace_back(QString::fromStdString(ext.variableName),
                                           QString::fromStdString(ext.sourcePath));
                }
            }
        }
    }
    actorConfig_.setPairs(std::move(config));
    actorAuthExtract_.setPairs(std::move(authEx));
    actorRefreshExtract_.setPairs(std::move(refreshEx));
    emit actorEditChanged();
}

bool AppController::saveActorEdits(const QString& originalId,
                                   const QString& name,
                                   const QString& strategyLabel,
                                   const QString& description,
                                   const QString& authMethod,
                                   const QString& authPath,
                                   const QString& authBody,
                                   const QString& authExpect,
                                   bool refreshEnabled,
                                   const QString& refreshMethod,
                                   const QString& refreshPath,
                                   const QString& refreshBody) {
    // Start from the existing actor (preserving inject headers, session TTL and
    // any chain steps beyond the first) when editing.
    engine::Actor actor;
    if (!originalId.isEmpty() && project_->hasProject()) {
        const auto& actors = project_->project().actors;
        const auto it = actors.find(engine::ActorId{originalId.toStdString()});
        if (it != actors.end()) {
            actor = it->second;
        }
    }
    actor.id = engine::ActorId{name.trimmed().toStdString()};
    actor.description = description.trimmed().toStdString();
    const engine::AuthStrategy strategy = authStrategyFromLabel(strategyLabel);
    actor.strategy = strategy;

    std::map<std::string, std::string> config;
    for (const auto& [key, value] : actorConfig_.pairs()) {
        if (!key.isEmpty()) {
            config.insert_or_assign(key.toStdString(), value.toStdString());
        }
    }
    actor.authConfig = std::move(config);

    const bool stepBased =
        (strategy == engine::AuthStrategy::Simple || strategy == engine::AuthStrategy::Chain);
    if (stepBased) {
        engine::AuthStep step;
        if (!actor.authSteps.empty()) {
            step = actor.authSteps.front();  // keep id + headers
        }
        if (step.id.empty()) {
            step.id = "login";
        }
        step.method = httpMethodFromLabel(authMethod);
        step.pathTemplate = authPath.trimmed().toStdString();
        const QString body = authBody.trimmed();
        step.bodyTemplate = body.isEmpty() ? std::optional<std::string>{}
                                           : std::optional<std::string>{body.toStdString()};
        bool okExpect = false;
        const int expect = authExpect.trimmed().toInt(&okExpect);
        step.expectStatus = okExpect ? std::optional<int>{expect} : std::optional<int>{};
        step.extractions = extractionsFromPairs(actorAuthExtract_.pairs());
        if (actor.authSteps.empty()) {
            actor.authSteps.push_back(std::move(step));
        } else {
            actor.authSteps.front() = std::move(step);
        }
    } else {
        actor.authSteps.clear();
    }

    if (refreshEnabled) {
        engine::SessionRefresh refresh;
        if (actor.refresh) {
            refresh = *actor.refresh;
        }
        refresh.method = httpMethodFromLabel(refreshMethod);
        refresh.pathTemplate = refreshPath.trimmed().toStdString();
        const QString body = refreshBody.trimmed();
        refresh.bodyTemplate = body.isEmpty() ? std::optional<std::string>{}
                                              : std::optional<std::string>{body.toStdString()};
        refresh.extractions = extractionsFromPairs(actorRefreshExtract_.pairs());
        actor.refresh = std::move(refresh);
    } else {
        actor.refresh.reset();
    }

    QString error;
    if (project_->saveActor(originalId, actor, error)) {
        emit notify(QStringLiteral("Saved actor “%1”").arg(name.trimmed()), false);
        return true;
    }
    emit notify(error, true);
    return false;
}

void AppController::deleteActor(const QString& actorId) {
    QString error;
    if (project_->deleteActor(engine::ActorId{actorId.toStdString()}, error)) {
        emit notify(QStringLiteral("Deleted actor “%1”").arg(actorId), false);
    } else {
        emit notify(error, true);
    }
}

void AppController::prepareNewEnvironment() {
    envVars_.clearRows();
    editEnvBaseUrl_.clear();
    emit editEnvBaseUrlChanged();
}

void AppController::setEditEnvBaseUrl(const QString& url) {
    if (url == editEnvBaseUrl_) {
        return;
    }
    editEnvBaseUrl_ = url;
    emit editEnvBaseUrlChanged();
}

void AppController::prepareEditEnvironment(const QString& name) {
    std::vector<std::pair<QString, QString>> pairs;
    editEnvBaseUrl_.clear();
    if (project_->hasProject()) {
        const auto& envs = project_->project().environments;
        const auto it = envs.find(name.toStdString());
        if (it != envs.end()) {
            for (const auto& [key, value] : it->second) {
                // baseUrl is surfaced in its own dedicated field, not the table.
                if (key == "baseUrl") {
                    editEnvBaseUrl_ = QString::fromStdString(value);
                    continue;
                }
                pairs.emplace_back(QString::fromStdString(key), QString::fromStdString(value));
            }
        }
    }
    envVars_.setPairs(std::move(pairs));
    emit editEnvBaseUrlChanged();
}

bool AppController::saveEnvironmentEdits(const QString& originalName, const QString& name) {
    std::map<std::string, std::string> variables;
    for (const auto& [key, value] : envVars_.pairs()) {
        // Skip a stray baseUrl row — the dedicated field is authoritative.
        if (!key.isEmpty() && key != QStringLiteral("baseUrl")) {
            variables.insert_or_assign(key.toStdString(), value.toStdString());
        }
    }
    const QString baseUrl = editEnvBaseUrl_.trimmed();
    if (!baseUrl.isEmpty()) {
        variables.insert_or_assign(std::string{"baseUrl"}, baseUrl.toStdString());
    }
    QString error;
    if (project_->saveEnvironment(originalName, name, variables, error)) {
        setEnvironment(name.trimmed());
        emit notify(QStringLiteral("Saved environment “%1”").arg(name.trimmed()), false);
        return true;
    }
    emit notify(error, true);
    return false;
}

void AppController::deleteEnvironment(const QString& name) {
    QString error;
    if (project_->deleteEnvironment(name, error)) {
        emit notify(QStringLiteral("Deleted environment “%1”").arg(name), false);
    } else {
        emit notify(error, true);
    }
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
