// AppController — see header.
#include "AppController.h"

#include "../../src/application/EnvironmentSettings.h"
#include "../../src/application/ProjectModel.h"
#include "../../src/application/WorkspaceModel.h"
#include "../../src/views/Formatting.h"
#include "../../src/views/HookEditorDialog.h"
#include "../../src/views/PathEval.h"
#include "../../src/widgets/GraphLayout.h"
#include "../../src/widgets/LineDiff.h"
#include "ThemeController.h"

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/FormBody.h>
#include <reqloom/engine/Predicate.h>

#include <QtConcurrent/QtConcurrentRun>

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QHash>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtCore/QStandardPaths>
#include <QtCore/QStringList>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QDialog>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <queue>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
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

/// When an OpenAPI import fails, sniff the file head for the common
/// non-OpenAPI shapes (Postman collection, Swagger 2.0) so the toast can
/// say something actionable instead of the engine's terse "found ''".
/// Returns an empty string when nothing recognizable is found, so the
/// caller falls back to the engine's own error detail.
[[nodiscard]] QString importFailureHint(const std::filesystem::path& spec) {
    std::ifstream in{spec, std::ios::binary};
    if (!in) {
        return {};
    }
    std::string head(8192, '\0');
    in.read(head.data(), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<std::size_t>(in.gcount()));

    if (head.find("schema.getpostman.com") != std::string::npos ||
        head.find("_postman_id") != std::string::npos) {
        // Postman is supported — but if the importer still failed, the export
        // is likely malformed or an unsupported schema version.
        return QStringLiteral(
            "This Postman collection couldn't be imported. Re-export it as Collection v2.1 "
            "and try again.");
    }
    if (head.find("\"swagger\"") != std::string::npos ||
        head.find("swagger:") != std::string::npos) {
        return QStringLiteral(
            "Swagger 2.0 isn't supported. Convert the spec to OpenAPI 3.x and try again.");
    }
    return {};
}

/// Cheap content sniff to pick the importer: a Postman collection export is
/// routed to the Postman parser, everything else to the OpenAPI parser.
[[nodiscard]] bool looksLikePostman(const std::filesystem::path& spec) {
    std::ifstream in{spec, std::ios::binary};
    if (!in) {
        return false;
    }
    std::string head(8192, '\0');
    in.read(head.data(), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<std::size_t>(in.gcount()));
    return head.find("schema.getpostman.com") != std::string::npos ||
           head.find("_postman_id") != std::string::npos;
}

/// Normalize a project directory path to a stable comparison key: strips a
/// trailing separator and resolves `.`/`..`/symlinks, so two spellings of the
/// same project (`/x/proj` and `/x/proj/`) collapse to one. `weakly_canonical`
/// keeps paths that no longer exist working.
[[nodiscard]] QString canonicalProjectPath(const QString& path) {
    if (path.isEmpty()) {
        return path;
    }
    std::error_code ec;
    const auto canon =
        std::filesystem::weakly_canonical(std::filesystem::path{path.toStdString()}, ec);
    return ec ? path : QString::fromStdString(canon.string());
}

/// Turn a project name into a filesystem-safe folder name: lowercase, runs of
/// non-alphanumeric characters collapse to a single '-', trimmed. So "RHP
/// Falls back to
/// "imported-project" when nothing usable remains.
[[nodiscard]] std::string projectFolderSlug(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    bool pendingDash = false;
    for (const char c : name) {
        const bool alnum =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (alnum) {
            if (pendingDash && !out.empty()) {
                out.push_back('-');
            }
            pendingDash = false;
            out.push_back(static_cast<char>((c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c));
        } else {
            pendingDash = true;
        }
    }
    return out.empty() ? std::string{"imported-project"} : out;
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
      workspace_(std::make_unique<WorkspaceModel>()),
      bootstrapper_(std::make_unique<Bootstrapper>()),
      runController_(
          std::make_unique<RunController>(bootstrapper_->engine(), *workspace_->active(), this)) {
    bindProject(workspace_->active());

    // Deliver an off-thread OpenAPI import's result back on the GUI thread:
    // load the written project and surface the summary / review notes here,
    // where touching models and emitting UI signals is safe.
    connect(&importWatcher_, &QFutureWatcher<ImportOutcome>::finished, this, [this]() {
        const ImportOutcome out = importWatcher_.result();
        switch (out.status) {
            case ImportOutcome::Status::ImportFailed:
                emit notify(tr("Import failed: %1").arg(out.errorDetail), true);
                return;
            case ImportOutcome::Status::WriteFailed:
                emit notify(tr("Could not write project: %1").arg(out.errorDetail), true);
                return;
            case ImportOutcome::Status::NeedsOverwrite:
                // The target project folder already exists — ask the UI to
                // confirm, then it re-invokes importOpenApi(..., overwrite=true).
                emit importNeedsOverwrite(QUrl::fromLocalFile(out.specPath),
                                          QUrl::fromLocalFile(out.baseDir));
                return;
            case ImportOutcome::Status::Success:
                break;
        }
        openProject(QUrl::fromLocalFile(out.loadDir));
        emit notify(
            tr("Imported %1 resources, %2 operations.").arg(out.resources).arg(out.operations),
            false);
        if (!out.notes.isEmpty()) {
            emit importReviewNotes(out.notes);
        }
    });

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
                responseBody_.setBody(body);
                emit responseChanged();
            });
    // Re-fetch auto-save: when the producer endpoint targeted by
    // refreshCandidates returns, store its body as an example so the value
    // picker's candidate list updates with the freshly fetched ids.
    connect(runController_.get(),
            &RunController::responseReceived,
            this,
            [this](int index, int status, const QString&, int, qint64, const QString& body) {
                if (pendingCandidateOp_.isEmpty()) {
                    return;
                }
                if (runStepOp_.value(index) != pendingCandidateOp_) {
                    return;
                }
                if (status >= 200 && status < 300 && !body.isEmpty()) {
                    SavedResponse example;
                    example.name = QStringLiteral("Latest (auto)");
                    example.status = status;
                    example.body = body;
                    exampleStore_.save(pendingCandidateOp_, example);
                }
                pendingCandidateOp_.clear();
                emit variableOverridesChanged();
            });
    connect(runController_.get(), &RunController::runEnded, this, [this](const QString& outcome) {
        runOutcome_ = outcome;
        emit responseChanged();
        // The run was just persisted; surface it in the history view.
        refreshHistory();
        // A run may have absorbed Set-Cookie headers into the actor jars.
        emit cookiesChanged();
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
    connect(runController_.get(),
            &RunController::assertionCompleted,
            &timeline_,
            &TimelineModel::onAssertionCompleted);
    connect(
        runController_.get(), &RunController::stepFailed, &timeline_, &TimelineModel::onStepFailed);
    connect(runController_.get(), &RunController::runEnded, &timeline_, &TimelineModel::onRunEnded);

    // Live per-node status for the chain graph. Run events carry an operation
    // by name (stepStarted / stepFailed) or only a step index (responseReceived);
    // we map index → op from stepStarted so every event can colour its node.
    connect(runController_.get(),
            &RunController::runStarted,
            this,
            [this](const QString&, int, const QString&) {
                runStepOp_.clear();
                chainStatus_.clear();
                emit chainStatusChanged();
            });
    connect(runController_.get(),
            &RunController::stepStarted,
            this,
            [this](int index, const QString& op, int) {
                runStepOp_.insert(index, op);
                chainStatus_.insert(op, QStringLiteral("running"));
                emit chainStatusChanged();
            });
    connect(runController_.get(),
            &RunController::stepSkipped,
            this,
            [this](int index, const QString& op, const QString&) {
                runStepOp_.insert(index, op);
                chainStatus_.insert(op, QStringLiteral("skipped"));
                emit chainStatusChanged();
            });
    connect(runController_.get(),
            &RunController::responseReceived,
            this,
            [this](int index, int status, const QString&, int, qint64, const QString&) {
                const QString op = runStepOp_.value(index);
                if (!op.isEmpty()) {
                    chainStatus_.insert(op,
                                        status >= 500   ? QStringLiteral("error")
                                        : status >= 300 ? QStringLiteral("warning")
                                                        : QStringLiteral("success"));
                    emit chainStatusChanged();
                }
            });
    connect(runController_.get(),
            &RunController::stepFailed,
            this,
            [this](int index, const QString& op, const QString&, const QString&) {
                runStepOp_.insert(index, op);
                chainStatus_.insert(op, QStringLiteral("error"));
                emit chainStatusChanged();
            });

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
                                      static_cast<QAbstractItemModel*>(&editExtractions_),
                                      static_cast<QAbstractItemModel*>(&editAssertions_)}) {
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

    // Keep the New Endpoint dialog's per-dependency extraction editors in sync
    // with its chosen dependencies.
    const auto onNewDepsChanged = [this]() {
        rebuildNewEndpointDepExtracts();
    };
    connect(&newEndpointDeps_, &QAbstractItemModel::dataChanged, this, onNewDepsChanged);
    connect(&newEndpointDeps_, &QAbstractItemModel::rowsInserted, this, onNewDepsChanged);
    connect(&newEndpointDeps_, &QAbstractItemModel::rowsRemoved, this, onNewDepsChanged);
    connect(&newEndpointDeps_, &QAbstractItemModel::modelReset, this, onNewDepsChanged);
    connect(&editDependencies_, &QAbstractItemModel::rowsRemoved, this, onDepsChanged);
    connect(&editDependencies_, &QAbstractItemModel::modelReset, this, onDepsChanged);

    restoreOpenProjects();
    refreshHistory();
}

AppController::~AppController() = default;

int AppController::resourceCount() const {
    return activeProject().hasProject()
               ? static_cast<int>(activeProject().project().resources.size())
               : 0;
}

int AppController::latencySloP95Ms() const {
    return activeProject().hasProject() ? activeProject().project().latencySloP95Ms : 0;
}

void AppController::setLatencySlo(int ms) {
    if (!activeProject().hasProject()) {
        emit notify(QStringLiteral("Open a project before setting a latency SLO."), true);
        return;
    }
    QString error;
    if (activeProject().setLatencySloP95Ms(ms, error)) {
        emit projectChanged();
        emit notify(ms > 0 ? QStringLiteral("Latency SLO set: p95 < %1 ms").arg(ms)
                           : QStringLiteral("Latency SLO cleared"),
                    false);
    } else {
        emit notify(error, true);
    }
}

void AppController::openProject(const QUrl& directory) {
    const QString path = directory.isLocalFile() ? directory.toLocalFile() : directory.toString();
    openProjectPath(path);
}

ProjectModel& AppController::activeProject() noexcept {
    return *workspace_->active();
}

const ProjectModel& AppController::activeProject() const noexcept {
    return *workspace_->active();
}

void AppController::bindProject(ProjectModel* project) {
    // UniqueConnection so re-binding the same project (e.g. via rebindActiveProject
    // on every activation) never stacks duplicate connections.
    connect(project, &ProjectModel::loaded, this, &AppController::onLoaded, Qt::UniqueConnection);
    connect(project, &ProjectModel::saved, this, &AppController::onSaved, Qt::UniqueConnection);
    connect(project,
            &ProjectModel::loadFailed,
            this,
            &AppController::onLoadFailed,
            Qt::UniqueConnection);
}

void AppController::openProjectPath(const QString& path) {
    if (path.isEmpty()) {
        return;
    }
    if (runController_->isRunning()) {
        emit notify(tr("Finish the current run before opening another project."), true);
        return;
    }

    // Canonicalize so dedup + persistence use a stable key that matches
    // ProjectModel::rootPath() after the load.
    std::error_code ec;
    const std::filesystem::path canon =
        std::filesystem::weakly_canonical(std::filesystem::path{path.toStdString()}, ec);
    const QString canonical = ec ? path : QString::fromStdString(canon.string());

    // Already open? Just activate it — no duplicate collections.
    if (const int existing = workspace_->indexOfRoot(canonical); existing >= 0) {
        activateProject(existing);
        return;
    }

    // Reuse the current slot when it's an empty sentinel (first open, or after
    // a failed load); otherwise add a new collection and activate it up-front
    // so the synchronous `loaded` rebinds to the right project.
    const int prevActive = workspace_->activeIndex();
    const bool reuse = !activeProject().hasProject();
    ProjectModel* target = nullptr;
    if (reuse) {
        target = workspace_->active();
    } else {
        target = workspace_->addProject();
        bindProject(target);
        workspace_->setActiveIndex(workspace_->count() - 1);
    }

    // Synchronous: emits `loaded` (→ onLoaded rebinds) or `loadFailed` (→ toast).
    target->loadFromDirectory(canonical);

    if (!target->hasProject() && !reuse) {
        // Load failed on a freshly added slot: drop it and rebind to the
        // project that was active before, so a bad open doesn't strand the UI.
        workspace_->removeProject(workspace_->count() - 1);
        workspace_->setActiveIndex(prevActive);
        rebindActiveProject(/*repopulateTree=*/true);
        selectFirstModule();
    }
}

void AppController::activateProject(int index) {
    if (index < 0 || index >= workspace_->count()) {
        return;
    }
    if (runController_->isRunning()) {
        emit notify(tr("Finish the current run before switching projects."), true);
        return;
    }
    workspace_->setActiveIndex(index);
    persistActiveProject();
    // The tree already lists every collection; just rebind the active views and
    // show the newly-active project's first module.
    rebindActiveProject(/*repopulateTree=*/false);
    selectFirstModule();
}

void AppController::closeProject(int index) {
    if (index < 0 || index >= workspace_->count()) {
        return;
    }
    const bool wasActive = index == workspace_->activeIndex();
    if (runController_->isRunning() && wasActive) {
        emit notify(tr("Finish the current run before closing this project."), true);
        return;
    }
    workspace_->removeProject(index);
    persistOpenProjects();
    persistActiveProject();
    if (wasActive) {
        // Active project changed (to a neighbour or a fresh empty slot) — rebuild
        // the tree (the closed subtree is gone) and rebind the UI to the new one.
        rebindActiveProject(/*repopulateTree=*/true);
        selectFirstModule();
    } else {
        // A background collection closed: drop its subtree from the tree.
        populateWorkspaceTree();
        emit openProjectsChanged();
    }
}

void AppController::populateWorkspaceTree() {
    // Feed every open (loaded) collection to the aggregated explorer tree. The
    // active flag drives the active-project highlight; ProjectRootRole on every
    // row lets a click resolve the owning project.
    std::vector<ProjectTreeModel::ProjectEntry> entries;
    for (int i = 0; i < workspace_->count(); ++i) {
        const ProjectModel* p = workspace_->at(i);
        if (p == nullptr || !p->hasProject()) {
            continue;
        }
        ProjectTreeModel::ProjectEntry entry;
        entry.root = p->rootPath();
        entry.name = p->name();
        entry.project = p->projectPtr();
        entry.active = i == workspace_->activeIndex();
        entries.push_back(std::move(entry));
    }
    tree_.populate(entries);
}

void AppController::selectFirstModule() {
    if (activeProject().hasProject() && !activeProject().project().resources.empty()) {
        selectModule(
            QString::fromStdString(activeProject().project().resources.begin()->first.value));
    }
}

void AppController::rebindActiveProject(bool repopulateTree) {
    // Ensure the active project's signals are connected. Cheap + idempotent
    // (UniqueConnection), and the single choke point that guarantees even a
    // freshly-minted sentinel (from closing the last project) is bound before
    // the next open/create loads into it.
    bindProject(workspace_->active());

    // Point the run controller at the active project first — every run + cookie
    // query resolves against it.
    runController_->setProject(activeProject());

    if (repopulateTree) {
        populateWorkspaceTree();
    }

    if (!activeProject().hasProject()) {
        // The active slot has no project (e.g. the last collection was closed).
        // Show the empty state rather than dereferencing an unloaded project.
        projectName_.clear();
        resources_.reset();
        operations_.reset();
        selectedModule_.clear();
        closeOperation();
        status_.clear();
        emit projectChanged();
        emit selectionChanged();
        emit openProjectsChanged();
        return;
    }

    projectName_ = activeProject().name();
    resources_.reload(activeProject().project());
    status_ = QStringLiteral("%1 modules").arg(resourceCount());
    // Point the saved-example store at this project (per-project isolation) and
    // re-apply the explorer's example child rows from disk.
    exampleStore_.setProjectRoot(activeProject().rootPath());
    // Open this project's own run-history database so runs are isolated between
    // projects. Keyed by a hash of the project root under the app-data dir.
    openProjectHistory(activeProject().rootPath());
    environments_ = activeProject().environmentNames();
    if (environment_.isEmpty() || !environments_.contains(environment_)) {
        // Restore the per-project saved environment first, then fall back to
        // the project default.
        QSettings settings;
        const QString saved = EnvironmentSettings::load(settings, activeProject().rootPath());
        if (!saved.isEmpty() && environments_.contains(saved)) {
            environment_ = saved;
        } else {
            environment_ = activeProject().defaultEnvironment();
            if (environment_.isEmpty() && !environments_.isEmpty()) {
                environment_ = environments_.front();
            }
        }
        emit environmentChanged();
    }
    // Reset the open operation — the previously-open op belonged to the prior
    // active project. Callers select what comes next (first module on a fresh
    // load / switcher activation; a specific op on a tree click).
    selectedModule_.clear();
    operations_.reset();
    closeOperation();
    refreshExamples();
    emit projectChanged();
    emit selectionChanged();
    emit openProjectsChanged();
}

bool AppController::activateForRow(const QString& projectRoot) {
    const int idx = workspace_->indexOfRoot(projectRoot);
    if (idx < 0) {
        return false;  // unknown collection
    }
    if (idx == workspace_->activeIndex()) {
        return true;  // already active — safe to proceed
    }
    if (runController_->isRunning()) {
        emit notify(tr("Finish the current run before switching projects."), true);
        return false;  // can't switch mid-run
    }
    workspace_->setActiveIndex(idx);
    persistActiveProject();
    // No tree repopulate (the tree already shows every collection; the active
    // highlight follows AppController.projectRoot reactively) and no
    // auto-select (the caller opens a specific operation).
    rebindActiveProject(/*repopulateTree=*/false);
    return true;
}

void AppController::selectOperationInProject(const QString& projectRoot,
                                             const QString& operationId) {
    if (!activateForRow(projectRoot)) {
        return;
    }
    selectOperationById(operationId);
}

void AppController::activateOperationInProject(const QString& projectRoot,
                                               const QString& operationId) {
    if (!activateForRow(projectRoot)) {
        return;
    }
    activateOperationById(operationId);
}

bool AppController::activateProjectByRoot(const QString& projectRoot) {
    return activateForRow(projectRoot);
}

void AppController::closeProjectByRoot(const QString& projectRoot) {
    const int idx = workspace_->indexOfRoot(projectRoot);
    if (idx >= 0) {
        closeProject(idx);
    }
}

QString AppController::projectRoot() const {
    return activeProject().rootPath();
}

QVariantList AppController::recentProjects() const {
    QSettings settings;
    const int count = settings.beginReadArray(QStringLiteral("workspace/recentProjects"));
    QVariantList out;
    out.reserve(count);
    QSet<QString> seen;
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        // Normalize so legacy entries stored with/without a trailing slash
        // collapse to one — otherwise the same project appears twice.
        const QString path =
            canonicalProjectPath(settings.value(QStringLiteral("path")).toString());
        // Skip empties, duplicates, and stale entries (project moved/deleted).
        if (path.isEmpty() || seen.contains(path) ||
            !QFileInfo::exists(path + QStringLiteral("/reqloom.yaml"))) {
            continue;
        }
        seen.insert(path);
        const QString name = settings.value(QStringLiteral("name")).toString();
        QVariantMap entry;
        entry.insert(QStringLiteral("name"), name.isEmpty() ? QFileInfo(path).fileName() : name);
        entry.insert(QStringLiteral("path"), path);
        out.append(entry);
    }
    settings.endArray();
    return out;
}

namespace {
// Persist a {name, path} list to the workspace recent-projects array, clearing
// any prior entries first so a shrunk list doesn't leave stale indices behind.
void writeRecentProjects(const QVariantList& entries) {
    QSettings settings;
    settings.remove(QStringLiteral("workspace/recentProjects"));
    settings.beginWriteArray(QStringLiteral("workspace/recentProjects"));
    for (int i = 0; i < entries.size(); ++i) {
        const auto entry = entries[i].toMap();
        settings.setArrayIndex(i);
        settings.setValue(QStringLiteral("name"), entry.value(QStringLiteral("name")));
        settings.setValue(QStringLiteral("path"), entry.value(QStringLiteral("path")));
    }
    settings.endArray();
    settings.sync();
}
}  // namespace

void AppController::recordRecentProject(const QString& name, const QString& path) {
    if (path.isEmpty()) {
        return;
    }
    // ponytail: capped at 15 entries — a flat rewrite is fine at this size; a
    // larger list would want an LRU structure instead of a linear rebuild.
    constexpr int kMaxRecent = 15;
    // Normalize so a re-open under a differently-spelled path (trailing slash,
    // `..`) doesn't create a second recent entry for the same project.
    const QString canonical = canonicalProjectPath(path);
    QVariantMap current;
    current.insert(QStringLiteral("name"), name.isEmpty() ? QFileInfo(canonical).fileName() : name);
    current.insert(QStringLiteral("path"), canonical);

    QVariantList merged;
    merged.append(current);
    for (const auto& v : recentProjects()) {
        if (merged.size() >= kMaxRecent) {
            break;
        }
        if (canonicalProjectPath(v.toMap().value(QStringLiteral("path")).toString()) != canonical) {
            merged.append(v);
        }
    }
    writeRecentProjects(merged);
    emit recentProjectsChanged();
}

void AppController::removeRecentProject(const QString& path) {
    QVariantList kept;
    for (const auto& v : recentProjects()) {
        if (v.toMap().value(QStringLiteral("path")).toString() != path) {
            kept.append(v);
        }
    }
    writeRecentProjects(kept);
    emit recentProjectsChanged();
}

QVariantList AppController::openProjects() {
    QVariantList out;
    for (int i = 0; i < workspace_->count(); ++i) {
        const ProjectModel* p = workspace_->at(i);
        if (p == nullptr || !p->hasProject()) {
            continue;  // skip the unloaded sentinel
        }
        QVariantMap entry;
        entry.insert(QStringLiteral("name"), p->name());
        entry.insert(QStringLiteral("path"), p->rootPath());
        entry.insert(QStringLiteral("index"), i);
        entry.insert(QStringLiteral("active"), i == workspace_->activeIndex());
        out.append(entry);
    }
    return out;
}

void AppController::persistOpenProjects() {
    QSettings settings;
    settings.remove(QStringLiteral("workspace/openProjects"));
    settings.beginWriteArray(QStringLiteral("workspace/openProjects"));
    int written = 0;
    for (int i = 0; i < workspace_->count(); ++i) {
        const ProjectModel* p = workspace_->at(i);
        if (p == nullptr || !p->hasProject()) {
            continue;
        }
        settings.setArrayIndex(written++);
        settings.setValue(QStringLiteral("name"), p->name());
        settings.setValue(QStringLiteral("path"), p->rootPath());
    }
    settings.endArray();
    settings.sync();
    emit openProjectsChanged();
}

void AppController::persistActiveProject() {
    QSettings settings;
    settings.setValue(QStringLiteral("workspace/activeProject"), activeProject().rootPath());
    settings.sync();
}

void AppController::restoreOpenProjects() {
    // Read the saved open set, dropping stale entries (project moved/deleted).
    QStringList paths;
    QString activePath;
    {
        QSettings settings;
        const int count = settings.beginReadArray(QStringLiteral("workspace/openProjects"));
        for (int i = 0; i < count; ++i) {
            settings.setArrayIndex(i);
            const QString p = settings.value(QStringLiteral("path")).toString();
            if (!p.isEmpty() && QFileInfo::exists(p + QStringLiteral("/reqloom.yaml"))) {
                paths.append(p);
            }
        }
        settings.endArray();
        activePath = settings.value(QStringLiteral("workspace/activeProject")).toString();
    }

    if (paths.isEmpty()) {
        // No saved workspace — fall back to the dev sample (if any) so a
        // first run / developer checkout still opens something.
        loadSampleIfPresent();
        return;
    }

    // Eager-load each saved collection. openProjectPath handles reuse of the
    // sentinel for the first, and appends the rest; a bad one is dropped
    // without aborting the others.
    for (const QString& p : paths) {
        openProjectPath(p);
    }
    if (const int idx = workspace_->indexOfRoot(activePath); idx >= 0) {
        activateProject(idx);
    }
}

void AppController::importOpenApi(const QUrl& specFile, const QUrl& targetDir, bool overwrite) {
    if (importWatcher_.isRunning()) {
        return;  // an import is already in flight
    }

    const QString specPath = specFile.isLocalFile() ? specFile.toLocalFile() : specFile.toString();
    if (specPath.isEmpty()) {
        emit notify(tr("Choose a spec or collection file to import."), true);
        return;
    }
    // The destination is optional: when omitted (the default flow), the project
    // is created next to the source file. A named sub-folder is always created
    // under the base, so we never scatter reqloom.yaml into the base directory
    // itself. `targetDir` is only supplied when re-invoked to overwrite.
    const QString dirPath =
        targetDir.isLocalFile() ? targetDir.toLocalFile() : targetDir.toString();

    // Parse + verify + write off the GUI thread (AGENTS.md threading rule): a
    // large spec can take noticeable time. These engine free functions touch
    // no shared engine state, so running them concurrently with the GUI is
    // safe. The worker captures only owned copies (no `this`); the result is
    // delivered back to the GUI thread by `importWatcher_`'s finished handler,
    // where the project load + toasts run.
    auto future = QtConcurrent::run([specStd = specPath.toStdString(),
                                     dirStd = dirPath.toStdString(),
                                     overwrite]() -> ImportOutcome {
        ImportOutcome out;
        const std::filesystem::path spec{specStd};
        out.specPath = QString::fromStdString(specStd);

        // Base directory the project folder is created under: the caller's
        // choice, else the source file's own directory.
        const std::filesystem::path base =
            dirStd.empty() ? spec.parent_path() : std::filesystem::path{dirStd};
        out.baseDir = QString::fromStdString(base.string());

        // Containment root: the spec's own directory, so an explicitly
        // chosen file always resolves inside it while the engine still
        // rejects `..` traversal.
        auto imported = looksLikePostman(spec)
                            ? engine::importFromPostman(spec, spec.parent_path())
                            : engine::importFromOpenApi(spec, spec.parent_path());
        if (!imported) {
            out.status = ImportOutcome::Status::ImportFailed;
            // Prefer an actionable hint for recognizable files (Postman /
            // Swagger 2.0); otherwise surface the engine detail.
            const QString hint = importFailureHint(spec);
            out.errorDetail =
                hint.isEmpty() ? QString::fromStdString(imported.error().detail) : hint;
            return out;
        }

        // Everything lands in a named sub-folder derived from the project name,
        // so importing never litters the chosen/base directory's root.
        const std::filesystem::path projectDir = base / projectFolderSlug(imported->project.name);

        std::error_code existsEc;
        if (!overwrite && std::filesystem::exists(projectDir / "reqloom.yaml", existsEc)) {
            out.status = ImportOutcome::Status::NeedsOverwrite;
            return out;
        }

        // writeProject doesn't create the target directory, so ensure it exists.
        std::error_code mkEc;
        std::filesystem::create_directories(projectDir, mkEc);

        auto written = engine::writeProject(projectDir, imported->project, overwrite);
        if (!written) {
            out.status = ImportOutcome::Status::WriteFailed;
            out.errorDetail = QString::fromStdString(written.error().detail);
            return out;
        }
        for (const auto& [resId, resource] : imported->project.resources) {
            out.operations += static_cast<int>(resource.operations.size());
        }
        out.status = ImportOutcome::Status::Success;
        out.resources = static_cast<int>(imported->project.resources.size());
        out.notes = QString::fromStdString(imported->warnings);
        out.loadDir = QString::fromStdString(projectDir.string());
        return out;
    });
    importWatcher_.setFuture(future);
}

void AppController::onLoaded() {
    // A fresh load: record it in recents + the open set, then rebind the UI
    // (rebuilding the aggregated tree) and open the project's first module.
    recordRecentProject(activeProject().name(), activeProject().rootPath());
    persistOpenProjects();
    persistActiveProject();
    rebindActiveProject(/*repopulateTree=*/true);
    selectFirstModule();
}

void AppController::onSaved() {
    // A mutation (operation/chain/actor/env save) re-publishes the project.
    // Unlike a fresh load, keep the user where they are: refresh project-derived
    // models but preserve the selected module + open operation so saving from
    // the response/chain editor doesn't bounce them back to the first module.
    resources_.reload(activeProject().project());
    populateWorkspaceTree();
    status_ = QStringLiteral("%1 modules").arg(resourceCount());
    exampleStore_.setProjectRoot(activeProject().rootPath());

    environments_ = activeProject().environmentNames();

    const QString openModule = selectedModule_;
    const QString openOp = hasOperation_ ? opName_ : QString{};
    if (!openModule.isEmpty()) {
        const auto& resources = activeProject().project().resources;
        const auto it = resources.find(engine::ResourceId{openModule.toStdString()});
        if (it != resources.end()) {
            operations_.reload(it->second);
        }
    }
    refreshExamples();
    emit projectChanged();
    emit selectionChanged();

    // Refresh the open operation's read fields from the saved project, but only
    // when not editing (re-selecting would discard an in-progress edit).
    if (!editing_ && !openModule.isEmpty() && !openOp.isEmpty()) {
        const auto& resources = activeProject().project().resources;
        const auto it = resources.find(engine::ResourceId{openModule.toStdString()});
        if (it != resources.end() && it->second.operations.contains(openOp.toStdString())) {
            selectOperation(openModule, openOp);
        }
    }
}

void AppController::selectModule(const QString& moduleName) {
    if (!activeProject().hasProject()) {
        return;
    }
    const auto& resources = activeProject().project().resources;
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
    if (!activeProject().hasProject()) {
        return;
    }
    const auto& resources = activeProject().project().resources;
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

    // Read-view assertions: key = label (name, or the expression when unnamed),
    // value = the expression. Reuses the KeyValueList widget.
    std::vector<std::pair<QString, QString>> assertRows;
    assertRows.reserve(op.assertions.size());
    for (const auto& a : op.assertions) {
        const QString expr = QString::fromStdString(a.expr);
        const QString label = a.name ? QString::fromStdString(*a.name) : expr;
        assertRows.emplace_back(label, expr);
    }
    opAssertions_.reloadPairs(std::move(assertRows));

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
    if (!activeProject().rootPath().isEmpty()) {
        QSettings settings;
        EnvironmentSettings::save(settings, activeProject().rootPath(), env);
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

QStringList AppController::extractedVariablesFor(const QString& operationId) const {
    QStringList tokens;
    if (!activeProject().hasProject()) {
        return tokens;
    }
    const auto* op = activeProject().findOperation(engine::OperationId{operationId.toStdString()});
    if (op == nullptr) {
        return tokens;
    }
    const qsizetype dot = operationId.indexOf(QLatin1Char('.'));
    const QString resource = dot > 0 ? operationId.left(dot) : QString{};
    for (const auto& ext : op->extractions) {
        const QString name = QString::fromStdString(ext.variableName);
        // variableName is the bare name; reference it namespaced by resource.
        const QString full = name.contains(QLatin1Char('.')) || resource.isEmpty()
                                 ? name
                                 : (resource + QLatin1Char('.') + name);
        tokens.append(QStringLiteral("{{%1}}").arg(full));
    }
    return tokens;
}

QVariantList AppController::extractionPairsFor(const QString& operationId) const {
    QVariantList pairs;
    if (!activeProject().hasProject()) {
        return pairs;
    }
    const auto* op = activeProject().findOperation(engine::OperationId{operationId.toStdString()});
    if (op == nullptr) {
        return pairs;
    }
    for (const auto& ext : op->extractions) {
        QVariantMap row;
        row.insert(QStringLiteral("key"), QString::fromStdString(ext.variableName));
        row.insert(QStringLiteral("value"), QString::fromStdString(ext.sourcePath));
        pairs.append(row);
    }
    return pairs;
}

QVariantList AppController::variableSuggestions(const QString& operationId) const {
    QVariantList out;
    if (!activeProject().hasProject() || !bootstrapper_ || operationId.isEmpty()) {
        return out;
    }
    auto result =
        bootstrapper_->engine().suggestVariables(activeProject().project(),
                                                 engine::OperationId{operationId.toStdString()},
                                                 environment_.toStdString());
    if (!result) {
        return out;
    }
    const auto kindString = [](engine::VariableSuggestion::Kind kind) -> QString {
        switch (kind) {
            case engine::VariableSuggestion::Kind::Extract:
                return QStringLiteral("extract");
            case engine::VariableSuggestion::Kind::ActorToken:
                return QStringLiteral("actor");
            case engine::VariableSuggestion::Kind::EnvVar:
                return QStringLiteral("env");
            case engine::VariableSuggestion::Kind::Secret:
                return QStringLiteral("secret");
            case engine::VariableSuggestion::Kind::Builtin:
                return QStringLiteral("builtin");
        }
        return {};
    };
    for (const auto& suggestion : *result) {
        QVariantMap entry;
        entry.insert(QStringLiteral("token"), QString::fromStdString(suggestion.token));
        entry.insert(QStringLiteral("kind"), kindString(suggestion.kind));
        entry.insert(QStringLiteral("detail"), QString::fromStdString(suggestion.detail));
        out.append(entry);
    }
    return out;
}

std::pair<QString, QString> AppController::findVariableProducer(const QString& token) const {
    if (!activeProject().hasProject() || token.isEmpty()) {
        return {};
    }
    const auto& proj = activeProject().project();
    for (const auto& [resId, resource] : proj.resources) {
        const QString resName = QString::fromStdString(resId.value);
        for (const auto& [opName, op] : resource.operations) {
            for (const auto& ext : op.extractions) {
                const QString var = QString::fromStdString(ext.variableName);
                // Match both the namespaced form (resource.var) and the bare
                // variable name (covers variables named with dots).
                if (token == (resName + QLatin1Char('.') + var) || token == var) {
                    return {resName + QLatin1Char('.') + QString::fromStdString(opName),
                            QString::fromStdString(ext.sourcePath)};
                }
            }
        }
    }
    return {};
}

QStringList AppController::candidateValues(const QString& token) const {
    QStringList out;
    const auto [producerOpId, sourcePath] = findVariableProducer(token);
    if (producerOpId.isEmpty() || sourcePath.isEmpty()) {
        return out;
    }

    // Turn a single-item extract path into its list form so every id surfaces:
    // `$.data[0].id` → `$.data[*].id`.
    static const QRegularExpression indexRe(QStringLiteral("\\[\\d+\\]"));
    QString listPath = sourcePath;
    listPath.replace(indexRe, QStringLiteral("[*]"));

    std::set<QString> seen;
    for (const auto& example : exampleStore_.list(producerOpId)) {
        for (const auto& value :
             engine::extractValues(example.body.toStdString(), listPath.toStdString())) {
            const QString candidate = QString::fromStdString(value);
            if (!candidate.isEmpty() && seen.insert(candidate).second) {
                out.append(candidate);
            }
        }
    }
    return out;
}

QString AppController::producerOpFor(const QString& token) const {
    return findVariableProducer(token).first;
}

QString AppController::responseBodyFor(const QString& operationId) const {
    if (operationId == currentOperationId() && !respBody_.isEmpty()) {
        return respBody_;
    }
    const QList<SavedResponse> saved = exampleStore_.list(operationId);
    return saved.isEmpty() ? QString() : saved.back().body;  // newest last
}

QVariantMap AppController::evaluateExtractionPath(const QString& operationId,
                                                  const QString& path) const {
    const QString body = responseBodyFor(operationId);
    const auto result =
        views::classifyExtractionPath(body.toStdString(), path.trimmed().toStdString());

    QVariantMap out;
    switch (result.state) {
        case views::PathState::Match:
            out.insert(QStringLiteral("state"), QStringLiteral("match"));
            out.insert(QStringLiteral("value"), QString::fromStdString(result.value).left(120));
            break;
        case views::PathState::NoMatch:
            out.insert(QStringLiteral("state"), QStringLiteral("nomatch"));
            break;
        case views::PathState::Neutral:
            out.insert(QStringLiteral("state"), QStringLiteral("neutral"));
            break;
    }
    return out;
}

QStringList AppController::suggestExtractionPaths(const QString& operationId,
                                                  const QString& prefix) const {
    return views::collectJsonPaths(responseBodyFor(operationId), prefix.trimmed());
}

void AppController::setVariableOverride(const QString& token, const QString& value) {
    if (token.isEmpty()) {
        return;
    }
    if (value.isEmpty()) {
        variableOverrides_.remove(token);
    } else {
        variableOverrides_.insert(token, value);
    }

    // Push the full pin set to the run controller and drop the extraction
    // cache so a removed/changed pin can't survive into the next run.
    std::vector<std::pair<std::string, std::string>> overrides;
    overrides.reserve(static_cast<std::size_t>(variableOverrides_.size()));
    for (auto it = variableOverrides_.constBegin(); it != variableOverrides_.constEnd(); ++it) {
        overrides.emplace_back(it.key().toStdString(), it.value().toStdString());
    }
    if (runController_) {
        runController_->setVariableOverrides(std::move(overrides));
        runController_->clearExtractionCache();
    }
    emit variableOverridesChanged();
}

QString AppController::variableOverride(const QString& token) const {
    return variableOverrides_.value(token);
}

void AppController::refreshCandidates(const QString& token) {
    if (!activeProject().hasProject() || runController_ == nullptr || runController_->isRunning()) {
        return;
    }
    const QString producerOpId = findVariableProducer(token).first;
    if (producerOpId.isEmpty()) {
        return;
    }
    // Capture is on by default; the response lands in the auto-save handler.
    pendingCandidateOp_ = producerOpId;
    runController_->run(producerOpId, environment_, false, false);
}

EditableKeyValueModel* AppController::chainExtractModelFor(const QString& operationId) {
    for (int i = 0; i < chainEditor_.count(); ++i) {
        if (chainEditor_.operationIdAt(i) == operationId) {
            return chainEditor_.extractModelAt(i);
        }
    }
    return nullptr;
}

QStringList AppController::chainForEachOptions(const QString& operationId) const {
    // A step can fan out over any resource produced by another step in the
    // chain. Offer the distinct resources of every other step, discovery order.
    QStringList resources;
    for (int i = 0; i < chainEditor_.count(); ++i) {
        const QString id = chainEditor_.operationIdAt(i);
        if (id == operationId) {
            continue;
        }
        const QString resource = id.section('.', 0, 0);
        if (!resource.isEmpty() && !resources.contains(resource)) {
            resources.append(resource);
        }
    }
    return resources;
}

QString AppController::chainForEachOver(const QString& operationId) const {
    for (int i = 0; i < chainEditor_.count(); ++i) {
        if (chainEditor_.operationIdAt(i) == operationId) {
            return chainEditor_.forEachOverAt(i);
        }
    }
    return {};
}

void AppController::chainSetForEach(const QString& operationId, const QString& overResource) {
    for (int i = 0; i < chainEditor_.count(); ++i) {
        if (chainEditor_.operationIdAt(i) == operationId) {
            chainEditor_.setForEachOver(i, overResource);
            return;
        }
    }
}

bool AppController::chainForEachContinueOnError(const QString& operationId) const {
    for (int i = 0; i < chainEditor_.count(); ++i) {
        if (chainEditor_.operationIdAt(i) == operationId) {
            return chainEditor_.forEachContinueOnErrorAt(i);
        }
    }
    return false;
}

void AppController::chainSetForEachContinueOnError(const QString& operationId,
                                                   bool continueOnError) {
    for (int i = 0; i < chainEditor_.count(); ++i) {
        if (chainEditor_.operationIdAt(i) == operationId) {
            chainEditor_.setForEachContinueOnError(i, continueOnError);
            return;
        }
    }
}

bool AppController::addExtraction(const QString& variableName, const QString& sourcePath) {
    if (!activeProject().hasProject() || !hasOperation_) {
        return false;
    }
    const QString opId = currentOperationId();
    const auto* op = activeProject().findOperation(engine::OperationId{opId.toStdString()});
    if (op == nullptr) {
        return false;
    }
    const QString var = variableName.trimmed();
    const QString path = sourcePath.trimmed();
    if (var.isEmpty() || path.isEmpty()) {
        emit notify(QStringLiteral("A variable name and a path are both required."), true);
        return false;
    }

    engine::Operation updated = *op;
    const std::string varStd = var.toStdString();
    const std::string pathStd = path.toStdString();
    // Replace a same-named extraction in place, otherwise append a new one.
    bool replaced = false;
    for (auto& ext : updated.extractions) {
        if (ext.variableName == varStd) {
            ext.sourcePath = pathStd;
            ext.source = sourceForPath(pathStd);
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        engine::Extraction extraction;
        extraction.variableName = varStd;
        extraction.sourcePath = pathStd;
        extraction.source = sourceForPath(pathStd);
        updated.extractions.push_back(std::move(extraction));
    }

    QString error;
    if (!activeProject().saveOperation(engine::OperationId{opId.toStdString()}, updated, error)) {
        emit notify(error.isEmpty() ? QStringLiteral("Could not save the variable.") : error, true);
        return false;
    }
    const qsizetype dot = opId.indexOf(QLatin1Char('.'));
    const QString resource = dot > 0 ? opId.left(dot) : opId;
    emit notify(QStringLiteral("Saved variable {{%1.%2}}").arg(resource, var), false);
    return true;
}

QVariantMap AppController::evaluateAssertion(const QString& expression) const {
    QVariantMap out;
    out.insert(QStringLiteral("valid"), false);
    out.insert(QStringLiteral("passed"), false);
    out.insert(QStringLiteral("error"), QString{});

    const QString trimmed = expression.trimmed();
    if (trimmed.isEmpty()) {
        return out;  // nothing to test; UI hides the badge for empty rows
    }

    auto result =
        engine::evaluatePredicate(trimmed.toStdString(), respBody_.toStdString(), respStatus_);
    if (!result) {
        out.insert(QStringLiteral("error"), QString::fromStdString(result.error().detail));
        return out;
    }
    out.insert(QStringLiteral("valid"), true);
    out.insert(QStringLiteral("passed"), *result);
    return out;
}

QVariantMap AppController::previewFormBody() const {
    QVariantMap out;

    std::map<std::string, std::string> fields;
    for (const auto& [key, value] : editForm_.pairs()) {
        const QString trimmedKey = key.trimmed();
        if (!trimmedKey.isEmpty()) {
            fields[trimmedKey.toStdString()] = value.toStdString();
        }
    }
    std::map<std::string, std::string> headers;
    for (const auto& [key, value] : editHeaders_.pairs()) {
        const QString trimmedKey = key.trimmed();
        if (!trimmedKey.isEmpty()) {
            headers[trimmedKey.toStdString()] = value.toStdString();
        }
    }

    auto preview = engine::previewFormBody(fields, headers);
    if (!preview) {
        out.insert(QStringLiteral("valid"), false);
        out.insert(QStringLiteral("error"), QString::fromStdString(preview.error().detail));
        return out;
    }

    out.insert(QStringLiteral("valid"), true);
    out.insert(QStringLiteral("error"), QString{});
    out.insert(QStringLiteral("multipart"), preview->kind == engine::FormBodyKind::Multipart);
    out.insert(QStringLiteral("contentType"), QString::fromStdString(preview->contentType));
    out.insert(QStringLiteral("totalBytes"), static_cast<qulonglong>(preview->totalBytes));
    QVariantList parts;
    for (const auto& part : preview->parts) {
        QVariantMap entry;
        entry.insert(QStringLiteral("name"), QString::fromStdString(part.name));
        entry.insert(QStringLiteral("isFile"), part.isFile);
        entry.insert(QStringLiteral("filename"), QString::fromStdString(part.filename));
        entry.insert(QStringLiteral("sizeBytes"), static_cast<qulonglong>(part.sizeBytes));
        parts.append(entry);
    }
    out.insert(QStringLiteral("parts"), parts);
    return out;
}

QVariantList AppController::cookieJars() const {
    QVariantList out;
    if (!activeProject().hasProject() || !runController_) {
        return out;
    }
    for (const auto& [actorId, _] : activeProject().project().actors) {
        const auto jar = runController_->cookies(actorId);
        if (jar.empty()) {
            continue;
        }
        QVariantList cookies;
        for (const auto& [name, value] : jar) {
            QVariantMap cookie;
            cookie.insert(QStringLiteral("name"), QString::fromStdString(name));
            cookie.insert(QStringLiteral("value"), QString::fromStdString(value));
            cookies.append(cookie);
        }
        QVariantMap entry;
        entry.insert(QStringLiteral("actor"), QString::fromStdString(actorId.value));
        entry.insert(QStringLiteral("cookies"), cookies);
        out.append(entry);
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

int AppController::editAssertionsCount() const {
    int count = 0;
    for (const auto& [expr, name] : editAssertions_.pairs()) {
        if (!expr.trimmed().isEmpty()) {
            ++count;
        }
    }
    return count;
}

QVariantList AppController::chainNodes() const {
    // Static view of the operation's declared dependencies in declared order,
    // then the target itself last (mirrors the old RequestEditorPanel chain
    // preview). In Edit mode the deps come from the live picker so the preview
    // updates as the user wires the chain. Implicit ({{var}}) deps and the
    // full topological order are resolved by the engine after a Dry Run.
    QVariantList nodes;
    if (!activeProject().hasProject() || !hasOperation_) {
        return nodes;
    }
    const QString opId = currentOperationId();
    const auto* op = activeProject().findOperation(engine::OperationId{opId.toStdString()});
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
        const auto* depOp = activeProject().findOperation(engine::OperationId{id.toStdString()});
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

QVariantMap AppController::chainGraph() const {
    // Draw the target's resolved dependency chain. The engine is the single
    // source of truth for resolution: resolvePlan() returns the topological
    // execution order plus the explicit/implicit edges (each implicit edge
    // tagged with the variable that flows along it). We only lay the result
    // out and translate it to the QML node/edge shape — no dependency
    // re-derivation here, so the drawn graph always matches what the engine
    // actually executes.
    QVariantMap graph;
    if (!activeProject().hasProject() || !hasOperation_ || !bootstrapper_) {
        return graph;
    }
    const QString targetId = currentOperationId();
    const engine::OperationId targetOpId{targetId.toStdString()};
    if (activeProject().findOperation(targetOpId) == nullptr) {
        return graph;
    }

    // In edit mode the chain picker holds unsaved depends_on edits. Resolve
    // against a patched copy so the preview tracks the live wiring; otherwise
    // resolve the persisted project directly.
    engine::Project patched;
    const engine::Project* proj = &activeProject().project();
    if (editing_ && chainFieldsLoaded_) {
        patched = activeProject().project();
        const qsizetype dot = targetId.indexOf(QLatin1Char('.'));
        if (dot > 0) {
            const engine::ResourceId resId{targetId.left(dot).toStdString()};
            const std::string opName = targetId.mid(dot + 1).toStdString();
            if (auto resIt = patched.resources.find(resId); resIt != patched.resources.end()) {
                if (auto opIt = resIt->second.operations.find(opName);
                    opIt != resIt->second.operations.end()) {
                    opIt->second.explicitDependencies.clear();
                    for (const auto& dep : editDependencies_.dependencies()) {
                        opIt->second.explicitDependencies.push_back(engine::OperationId{dep});
                    }
                }
            }
        }
        proj = &patched;
    }

    const auto plan = bootstrapper_->engine().resolvePlan(*proj, targetOpId);
    if (!plan || plan->order.size() <= 1) {
        // No chain to draw (or an edit-time cycle); ChainView shows empty text.
        return graph;
    }

    // Node index = position in the engine's topological order (deps first,
    // target last).
    QHash<QString, int> indexOf;
    QStringList ids;
    ids.reserve(static_cast<qsizetype>(plan->order.size()));
    for (const auto& opId : plan->order) {
        const QString id = QString::fromStdString(opId.value);
        indexOf.insert(id, static_cast<int>(ids.size()));
        ids.append(id);
    }

    // Aggregate engine edges by (producer → consumer) pair so a pair draws
    // once, not in parallel: an explicit edge wins (solid, no label); otherwise
    // join the implicit edges' flowing variables into one labeled edge.
    struct EdgeAgg {
        bool isExplicit{false};
        QStringList vars;
    };
    std::map<std::pair<int, int>, EdgeAgg> edgeAgg;
    QHash<QString, QStringList> depsOf;  // consumer id → producer ids
    for (const auto& edge : plan->edges) {
        const QString consumer = QString::fromStdString(edge.consumer.value);
        const QString producer = QString::fromStdString(edge.producer.value);
        const auto cIt = indexOf.constFind(consumer);
        const auto pIt = indexOf.constFind(producer);
        if (cIt == indexOf.constEnd() || pIt == indexOf.constEnd()) {
            continue;
        }
        // Layout/edge direction is prerequisite → dependent: from=producer.
        EdgeAgg& agg = edgeAgg[std::pair<int, int>{pIt.value(), cIt.value()}];
        if (edge.implicit) {
            if (!edge.variable.empty()) {
                agg.vars.append(
                    QStringLiteral("{{%1}}").arg(QString::fromStdString(edge.variable)));
            }
        } else {
            agg.isExplicit = true;
        }
        QStringList& producers = depsOf[consumer];
        if (!producers.contains(producer)) {
            producers.append(producer);
        }
    }

    std::vector<std::pair<int, int>> layoutEdges;
    layoutEdges.reserve(edgeAgg.size());
    for (const auto& [key, agg] : edgeAgg) {
        layoutEdges.push_back(key);
    }

    layout::LayoutOptions options;
    options.nodeWidth = 200.0;
    options.nodeHeight = 38.0;
    options.hGap = 20.0;
    options.vGap = 36.0;
    const layout::LayoutResult laid =
        layout::layeredLayout(static_cast<int>(ids.size()), layoutEdges, options);

    // Resolve a node's operation against the same project the plan came from,
    // so an edit-mode target reflects the patched copy.
    const auto findOp = [proj](const QString& id) -> const engine::Operation* {
        const qsizetype dot = id.indexOf(QLatin1Char('.'));
        if (dot <= 0) {
            return nullptr;
        }
        const auto resIt = proj->resources.find(engine::ResourceId{id.left(dot).toStdString()});
        if (resIt == proj->resources.end()) {
            return nullptr;
        }
        const auto opIt = resIt->second.operations.find(id.mid(dot + 1).toStdString());
        return opIt == resIt->second.operations.end() ? nullptr : &opIt->second;
    };

    QVariantList nodeList;
    for (int i = 0; i < ids.size(); ++i) {
        const QString& id = ids.at(i);
        const auto* op = findOp(id);
        QVariantMap node;
        node.insert(QStringLiteral("operationId"), id);
        node.insert(QStringLiteral("method"), op != nullptr ? methodLabel(op->method) : QString{});
        node.insert(QStringLiteral("isTarget"), id == targetId);
        node.insert(QStringLiteral("x"), laid.nodes[static_cast<std::size_t>(i)].x);
        node.insert(QStringLiteral("y"), laid.nodes[static_cast<std::size_t>(i)].y);
        // Detail surfaced when a node is clicked in the graph.
        node.insert(QStringLiteral("path"),
                    op != nullptr ? QString::fromStdString(op->pathTemplate) : QString{});
        node.insert(QStringLiteral("actor"),
                    op != nullptr ? QString::fromStdString(op->actor.value) : QString{});
        QStringList extracts;
        if (op != nullptr) {
            for (const auto& extraction : op->extractions) {
                extracts.append(QString::fromStdString(extraction.variableName));
            }
        }
        node.insert(QStringLiteral("extracts"), extracts);
        QStringList deps = depsOf.value(id);
        deps.sort();
        node.insert(QStringLiteral("deps"), deps);
        nodeList.append(node);
    }

    QVariantList edgeList;
    for (auto& [key, agg] : edgeAgg) {
        agg.vars.sort();
        QVariantMap edge;
        edge.insert(QStringLiteral("from"), key.first);
        edge.insert(QStringLiteral("to"), key.second);
        edge.insert(QStringLiteral("explicit"), agg.isExplicit);
        edge.insert(QStringLiteral("label"),
                    agg.isExplicit ? QString{} : agg.vars.join(QStringLiteral(", ")));
        edgeList.append(edge);
    }

    graph.insert(QStringLiteral("nodes"), nodeList);
    graph.insert(QStringLiteral("edges"), edgeList);
    graph.insert(QStringLiteral("width"), laid.width);
    graph.insert(QStringLiteral("height"), laid.height);
    graph.insert(QStringLiteral("nodeWidth"), options.nodeWidth);
    graph.insert(QStringLiteral("nodeHeight"), options.nodeHeight);
    return graph;
}

QVariantMap AppController::chainStatus() const {
    QVariantMap status;
    for (auto it = chainStatus_.constBegin(); it != chainStatus_.constEnd(); ++it) {
        status.insert(it.key(), it.value());
    }
    return status;
}

void AppController::prepareChainEditor() {
    if (!activeProject().hasProject() || !hasOperation_) {
        chainEditor_.rebuild({});
        return;
    }
    const QString targetId = currentOperationId();
    const auto* targetOp =
        activeProject().findOperation(engine::OperationId{targetId.toStdString()});
    if (targetOp == nullptr) {
        chainEditor_.rebuild({});
        return;
    }

    // BFS the declared transitive dependency closure, in discovery order.
    QStringList ids;
    QHash<QString, bool> seen;
    std::queue<QString> pending;
    ids.append(targetId);
    seen.insert(targetId, true);
    pending.push(targetId);
    while (!pending.empty()) {
        const QString id = pending.front();
        pending.pop();
        const auto* op = activeProject().findOperation(engine::OperationId{id.toStdString()});
        if (op == nullptr) {
            continue;
        }
        for (const auto& dep : op->explicitDependencies) {
            const QString depId = QString::fromStdString(dep.value);
            if (!seen.contains(depId)) {
                seen.insert(depId, true);
                ids.append(depId);
                pending.push(depId);
            }
        }
    }

    const QStringList allIds = operationIds();
    std::vector<ChainEditorModel::OpSeed> seeds;
    seeds.reserve(static_cast<std::size_t>(ids.size()));
    for (const QString& id : ids) {
        const auto* op = activeProject().findOperation(engine::OperationId{id.toStdString()});
        ChainEditorModel::OpSeed seed;
        seed.operationId = id;
        seed.method = op != nullptr ? methodLabel(op->method) : QString{};
        seed.isTarget = (id == targetId);
        if (op != nullptr) {
            for (const auto& dep : op->explicitDependencies) {
                seed.dependencies.push_back(dep.value);
            }
            for (const auto& ext : op->extractions) {
                seed.extractions.emplace_back(QString::fromStdString(ext.variableName),
                                              QString::fromStdString(ext.sourcePath));
            }
            if (op->forEach) {
                seed.forEachOver = QString::fromStdString(op->forEach->over.value);
                seed.forEachContinueOnError = op->forEach->continueOnError;
            }
        }
        for (const QString& candidate : allIds) {
            if (candidate != id) {
                seed.candidates.append(candidate);
            }
        }
        seeds.push_back(std::move(seed));
    }
    chainEditor_.rebuild(seeds);
}

void AppController::syncChainEditorMembership() {
    if (!activeProject().hasProject() || !hasOperation_ || chainEditor_.count() == 0) {
        return;
    }
    const QString targetId = currentOperationId();

    // Snapshot current edits so a rebuild preserves in-progress work.
    QHash<QString, std::vector<std::string>> editedDeps;
    QHash<QString, std::vector<std::pair<QString, QString>>> editedExtracts;
    QHash<QString, QString> editedForEach;
    QHash<QString, bool> editedForEachContinue;
    for (int i = 0; i < chainEditor_.count(); ++i) {
        const QString id = chainEditor_.operationIdAt(i);
        editedDeps.insert(id, chainEditor_.depModelAt(i)->dependencies());
        editedExtracts.insert(id, chainEditor_.extractModelAt(i)->pairs());
        editedForEach.insert(id, chainEditor_.forEachOverAt(i));
        editedForEachContinue.insert(id, chainEditor_.forEachContinueOnErrorAt(i));
    }

    // BFS the transitive closure from the target using edited deps where we
    // have them, falling back to the saved project for not-yet-edited steps.
    QStringList ids;
    QHash<QString, bool> seen;
    std::queue<QString> pending;
    ids.append(targetId);
    seen.insert(targetId, true);
    pending.push(targetId);
    while (!pending.empty()) {
        const QString id = pending.front();
        pending.pop();
        std::vector<std::string> deps;
        if (editedDeps.contains(id)) {
            deps = editedDeps.value(id);
        } else if (const auto* op =
                       activeProject().findOperation(engine::OperationId{id.toStdString()})) {
            for (const auto& dep : op->explicitDependencies) {
                deps.push_back(dep.value);
            }
        }
        for (const auto& dep : deps) {
            const QString depId = QString::fromStdString(dep);
            if (!seen.contains(depId)) {
                seen.insert(depId, true);
                ids.append(depId);
                pending.push(depId);
            }
        }
    }

    // Membership unchanged → live models are already correct, skip the rebuild
    // (avoids resetting focus and re-entrancy).
    QSet<QString> current;
    for (int i = 0; i < chainEditor_.count(); ++i) {
        current.insert(chainEditor_.operationIdAt(i));
    }
    const QSet<QString> wanted(ids.cbegin(), ids.cend());
    if (current == wanted) {
        return;
    }

    const QStringList allIds = operationIds();
    std::vector<ChainEditorModel::OpSeed> seeds;
    seeds.reserve(static_cast<std::size_t>(ids.size()));
    for (const QString& id : ids) {
        const auto* op = activeProject().findOperation(engine::OperationId{id.toStdString()});
        ChainEditorModel::OpSeed seed;
        seed.operationId = id;
        seed.method = op != nullptr ? methodLabel(op->method) : QString{};
        seed.isTarget = (id == targetId);
        if (editedDeps.contains(id)) {
            seed.dependencies = editedDeps.value(id);
        } else if (op != nullptr) {
            for (const auto& dep : op->explicitDependencies) {
                seed.dependencies.push_back(dep.value);
            }
        }
        if (editedExtracts.contains(id)) {
            seed.extractions = editedExtracts.value(id);
        } else if (op != nullptr) {
            for (const auto& ext : op->extractions) {
                seed.extractions.emplace_back(QString::fromStdString(ext.variableName),
                                              QString::fromStdString(ext.sourcePath));
            }
        }
        if (editedForEach.contains(id)) {
            seed.forEachOver = editedForEach.value(id);
        } else if (op != nullptr && op->forEach) {
            seed.forEachOver = QString::fromStdString(op->forEach->over.value);
        }
        if (editedForEachContinue.contains(id)) {
            seed.forEachContinueOnError = editedForEachContinue.value(id);
        } else if (op != nullptr && op->forEach) {
            seed.forEachContinueOnError = op->forEach->continueOnError;
        }
        for (const QString& candidate : allIds) {
            if (candidate != id) {
                seed.candidates.append(candidate);
            }
        }
        seeds.push_back(std::move(seed));
    }
    chainEditor_.rebuild(seeds);
}

void AppController::chainAddDependency(const QString& operationId) {
    if (operationId.isEmpty() || chainEditor_.count() == 0) {
        return;
    }
    const QString targetId = currentOperationId();
    for (int i = 0; i < chainEditor_.count(); ++i) {
        if (chainEditor_.operationIdAt(i) == targetId) {
            auto* deps = chainEditor_.depModelAt(i);
            const int ghost = deps->rowCount() - 1;
            deps->setSelection(ghost >= 0 ? ghost : 0, operationId);
            break;
        }
    }
    syncChainEditorMembership();
}

void AppController::chainRemoveStep(const QString& operationId) {
    if (operationId.isEmpty()) {
        return;
    }
    // Drop it as a dependency of every step so it is no longer referenced.
    for (int i = 0; i < chainEditor_.count(); ++i) {
        auto* deps = chainEditor_.depModelAt(i);
        for (int row = 0; row < deps->rowCount(); ++row) {
            const QString value =
                deps->data(deps->index(row, 0), DependencyEditModel::ValueRole).toString();
            if (value == operationId) {
                deps->removeRow(row);
                break;
            }
        }
    }
    syncChainEditorMembership();
}

bool AppController::saveChainEdits() {
    if (!activeProject().hasProject()) {
        return false;
    }
    std::map<std::string, engine::Operation> updates;
    for (int i = 0; i < chainEditor_.count(); ++i) {
        const QString id = chainEditor_.operationIdAt(i);
        const auto* op = activeProject().findOperation(engine::OperationId{id.toStdString()});
        if (op == nullptr) {
            continue;
        }
        engine::Operation updated = *op;  // copy; patch only the chain fields

        updated.explicitDependencies.clear();
        for (const auto& dep : chainEditor_.depModelAt(i)->dependencies()) {
            updated.explicitDependencies.push_back(engine::OperationId{dep});
        }

        // Preserve each extraction's source kind by variable name; new rows
        // default to JSONPath (the common case).
        std::map<std::string, engine::Extraction::Source> sourceByVar;
        for (const auto& ext : op->extractions) {
            sourceByVar[ext.variableName] = ext.source;
        }
        updated.extractions.clear();
        for (const auto& [variable, sourcePath] : chainEditor_.extractModelAt(i)->pairs()) {
            const QString var = variable.trimmed();
            const QString path = sourcePath.trimmed();
            if (var.isEmpty() && path.isEmpty()) {
                continue;
            }
            engine::Extraction extraction;
            extraction.variableName = var.toStdString();
            extraction.sourcePath = path.toStdString();
            const auto found = sourceByVar.find(extraction.variableName);
            extraction.source =
                found != sourceByVar.end() ? found->second : engine::Extraction::Source::JsonPath;
            updated.extractions.push_back(std::move(extraction));
        }

        // For-each fan-out: set or clear based on the chain editor's choice.
        const QString overResource = chainEditor_.forEachOverAt(i).trimmed();
        if (overResource.isEmpty()) {
            updated.forEach.reset();
        } else {
            engine::ForEach forEach{engine::ResourceId{overResource.toStdString()}};
            forEach.continueOnError = chainEditor_.forEachContinueOnErrorAt(i);
            updated.forEach = forEach;
        }

        updates.emplace(id.toStdString(), std::move(updated));
    }

    QString error;
    if (!activeProject().saveOperations(updates, error)) {
        emit notify(error.isEmpty() ? QStringLiteral("Could not save the chain.") : error, true);
        return false;
    }
    emit notify(QStringLiteral("Chain saved."), false);

    // Save chain rewrote the target's depends_on / extract on disk; refresh the
    // endpoint editor's edit-mode models so a later endpoint Save (which writes
    // from those models) reflects — rather than clobbers — what we just saved.
    if (editing_) {
        seedEditChainFromProject();
        emit editChanged();
        emit chainChanged();
    }
    return true;
}

void AppController::seedEditChainFromProject() {
    if (!activeProject().hasProject()) {
        return;
    }
    const auto* op =
        activeProject().findOperation(engine::OperationId{currentOperationId().toStdString()});
    if (op == nullptr) {
        return;
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

    std::vector<std::pair<QString, QString>> assertRows;
    assertRows.reserve(op->assertions.size());
    for (const auto& a : op->assertions) {
        assertRows.emplace_back(QString::fromStdString(a.expr),
                                a.name ? QString::fromStdString(*a.name) : QString{});
    }
    editAssertions_.setPairs(std::move(assertRows));
}

void AppController::beginEdit() {
    if (!hasOperation_ || !activeProject().hasProject()) {
        return;
    }
    const auto* op =
        activeProject().findOperation(engine::OperationId{currentOperationId().toStdString()});
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

    std::vector<std::pair<QString, QString>> assertRows;
    assertRows.reserve(op->assertions.size());
    for (const auto& a : op->assertions) {
        assertRows.emplace_back(QString::fromStdString(a.expr),
                                a.name ? QString::fromStdString(*a.name) : QString{});
    }
    editAssertions_.setPairs(std::move(assertRows));

    chainFieldsLoaded_ = true;
    editing_ = true;
    // Seed the whole-chain editor (every step in the transitive chain) so the
    // Chain tab can edit them all in one place.
    prepareChainEditor();
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
    for (const auto& [expr, name] : editAssertions_.pairs()) {
        const QString trimmedExpr = expr.trimmed();
        if (trimmedExpr.isEmpty()) {
            continue;
        }
        engine::Assertion assertion;
        assertion.expr = trimmedExpr.toStdString();
        const QString trimmedName = name.trimmed();
        if (!trimmedName.isEmpty()) {
            assertion.name = trimmedName.toStdString();
        }
        ov.assertions.push_back(std::move(assertion));
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
    if (!hasOperation_ || !activeProject().hasProject()) {
        return;
    }
    const QString opId = currentOperationId();
    const engine::OperationId id{opId.toStdString()};
    const auto* op = activeProject().findOperation(id);
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
    if (activeProject().saveOperation(id, updated, error)) {
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
    const auto* op = activeProject().findOperation(id);
    if (op == nullptr) {
        emit notify(QStringLiteral("No operation to edit hooks for"), true);
        return;
    }

    // Refresh the hook sandbox type definitions so a TS-aware editor offers
    // ctx.* autocomplete on any ./hooks/*.js this operation references.
    refreshHookTypings();

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
    const QString root = activeProject().rootPath();
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
    if (activeProject().saveOperation(id, updated, error)) {
        // saveOperation rebinds the project (its `saved` signal resets the
        // selection to the endpoint list), so reopen the operation to keep the
        // user where they were.
        selectOperationById(opId);
        emit notify(QStringLiteral("Saved hooks for “%1”").arg(opId), false);
    } else {
        emit notify(error, true);
    }
}

void AppController::refreshHookTypings() {
    if (!activeProject().hasProject()) {
        return;
    }
    const auto written =
        engine::emitHookTypings(std::filesystem::path{activeProject().rootPath().toStdString()},
                                activeProject().project(),
                                /*overwrite=*/true);
    if (!written) {
        // Best-effort: typings are a convenience, never a blocker for editing.
        return;
    }
}

void AppController::generateHookTypings() {
    if (!activeProject().hasProject()) {
        emit notify(QStringLiteral("Open a project before generating hook typings"), true);
        return;
    }
    const auto written =
        engine::emitHookTypings(std::filesystem::path{activeProject().rootPath().toStdString()},
                                activeProject().project(),
                                /*overwrite=*/true);
    if (!written) {
        emit notify(tr("Couldn't write hook typings: %1")
                        .arg(QString::fromStdString(written.error().detail)),
                    true);
        return;
    }
    emit notify(tr("Wrote %1").arg(QString::fromStdString(written->filename().string())), false);
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
    QMap<QString, QList<ProjectTreeModel::ExampleRow>> byOperation;
    // Scope example rows to the active project so identically-named operations
    // in other open collections don't inherit them.
    const QString activeRoot = activeProject().rootPath();
    for (const QString& id : exampleStore_.operationIds()) {
        QList<ProjectTreeModel::ExampleRow> rows;
        for (const SavedResponse& r : exampleStore_.list(id)) {
            rows.append(ProjectTreeModel::ExampleRow{r.name, r.status});
        }
        byOperation.insert(ProjectTreeModel::exampleKey(activeRoot, id), rows);
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
    selectedModule_.clear();
    // Keep any other open collections visible — only the failed slot is empty.
    populateWorkspaceTree();
    status_ = QStringLiteral("Load failed (%1): %2").arg(code, detail);
    emit projectChanged();
    emit selectionChanged();
}

void AppController::loadSampleIfPresent() {
    if (const QString sample = locateSampleProject(); !sample.isEmpty()) {
        activeProject().loadFromDirectory(sample);
    }
}

int AppController::operationCount() const {
    if (!activeProject().hasProject()) {
        return 0;
    }
    int count = 0;
    for (const auto& [resId, resource] : activeProject().project().resources) {
        count += static_cast<int>(resource.operations.size());
    }
    return count;
}

int AppController::actorCount() const {
    return activeProject().hasProject() ? static_cast<int>(activeProject().project().actors.size())
                                        : 0;
}

QStringList AppController::moduleNames() const {
    QStringList names;
    if (activeProject().hasProject()) {
        for (const auto& [resId, resource] : activeProject().project().resources) {
            names.append(QString::fromStdString(resId.value));
        }
    }
    return names;
}

QStringList AppController::actorNames() const {
    QStringList names;
    if (activeProject().hasProject()) {
        for (const auto& [actorId, actor] : activeProject().project().actors) {
            names.append(QString::fromStdString(actorId.value));
        }
    }
    return names;
}

QString AppController::actorAuthLabel(const QString& actorName) const {
    if (actorName.isEmpty()) {
        return QStringLiteral("No authentication");
    }
    if (!activeProject().hasProject()) {
        return {};
    }
    const auto& actors = activeProject().project().actors;
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
    if (!activeProject().hasProject()) {
        return {};
    }
    const auto& actors = activeProject().project().actors;
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

    if (activeProject().hasProject()) {
        const auto& actors = activeProject().project().actors;
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
    if (!originalId.isEmpty() && activeProject().hasProject()) {
        const auto& actors = activeProject().project().actors;
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
    if (activeProject().saveActor(originalId, actor, error)) {
        emit notify(QStringLiteral("Saved actor “%1”").arg(name.trimmed()), false);
        return true;
    }
    emit notify(error, true);
    return false;
}

void AppController::deleteActor(const QString& actorId) {
    QString error;
    if (activeProject().deleteActor(engine::ActorId{actorId.toStdString()}, error)) {
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
    if (activeProject().hasProject()) {
        const auto& envs = activeProject().project().environments;
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
    if (activeProject().saveEnvironment(originalName, name, variables, error)) {
        setEnvironment(name.trimmed());
        emit notify(QStringLiteral("Saved environment “%1”").arg(name.trimmed()), false);
        return true;
    }
    emit notify(error, true);
    return false;
}

void AppController::deleteEnvironment(const QString& name) {
    QString error;
    if (activeProject().deleteEnvironment(name, error)) {
        emit notify(QStringLiteral("Deleted environment “%1”").arg(name), false);
    } else {
        emit notify(error, true);
    }
}

void AppController::openProjectHistory(const QString& projectRoot) {
    if (!bootstrapper_) {
        return;
    }
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty() || projectRoot.isEmpty()) {
        return;
    }
    // Hash the canonical project path so each project maps to a stable,
    // filesystem-safe database name without leaking the path or colliding.
    const QString canonical = QFileInfo(projectRoot).canonicalFilePath();
    const QString key = canonical.isEmpty() ? projectRoot : canonical;
    const QString digest = QString::fromLatin1(
        QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex());
    const QString dir = QDir{base}.filePath(QStringLiteral("history"));
    QDir{}.mkpath(dir);
    const QString dbPath = QDir{dir}.filePath(QStringLiteral("history-%1.db").arg(digest));
    auto opened = bootstrapper_->engine().openHistory(std::filesystem::path{dbPath.toStdString()});
    if (!opened) {
        emit notify(QStringLiteral("Could not open run history: %1")
                        .arg(QString::fromStdString(opened.error().detail)),
                    true);
    }
}

void AppController::refreshHistory() {
    if (!bootstrapper_) {
        return;
    }
    auto runs = bootstrapper_->engine().listRuns(100);
    if (!runs) {
        // History is best-effort UI; an unopened/unavailable store just
        // shows an empty list rather than surfacing an error to the user.
        history_.reset();
        return;
    }
    history_.reload(*runs);
}

void AppController::clearHistory() {
    if (!bootstrapper_) {
        return;
    }
    auto cleared = bootstrapper_->engine().clearHistory();
    if (!cleared) {
        emit notify(QString::fromStdString(cleared.error().detail), true);
        return;
    }
    refreshHistory();
    emit notify(QStringLiteral("Run history cleared"), false);
}

void AppController::replayRun(qulonglong runId) {
    if (!bootstrapper_) {
        return;
    }
    auto events = bootstrapper_->engine().historyEvents(engine::RunId{runId});
    if (!events) {
        emit notify(QString::fromStdString(events.error().detail), true);
        return;
    }

    // Rebuild the live timeline from the persisted events, reusing the same
    // formatting the live run path applies so a replayed run reads identically.
    timeline_.reset();
    const auto joinHeaders = [](const std::vector<std::pair<std::string, std::string>>& headers) {
        QString out;
        for (const auto& [key, value] : headers) {
            if (!out.isEmpty()) {
                out.append(QLatin1Char('\n'));
            }
            out.append(QString::fromStdString(key));
            out.append(QStringLiteral(": "));
            out.append(QString::fromStdString(value));
        }
        return out;
    };

    for (const auto& event : *events) {
        std::visit(
            [&](const auto& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, engine::RunStarted>) {
                    timeline_.onRunStarted(QString::fromStdString(e.target.value),
                                           static_cast<int>(e.chainSize),
                                           QString::fromStdString(e.envName));
                } else if constexpr (std::is_same_v<T, engine::StepStarted>) {
                    timeline_.onStepStarted(static_cast<int>(e.stepIndex),
                                            QString::fromStdString(e.op.value),
                                            e.attempt);
                } else if constexpr (std::is_same_v<T, engine::StepSkipped>) {
                    timeline_.onStepSkipped(static_cast<int>(e.stepIndex),
                                            QString::fromStdString(e.op.value),
                                            format::skipReason(e.reason));
                } else if constexpr (std::is_same_v<T, engine::RequestPrepared>) {
                    timeline_.onRequestPrepared(static_cast<int>(e.stepIndex),
                                                format::method(e.method),
                                                QString::fromStdString(e.url),
                                                joinHeaders(e.maskedHeaders),
                                                static_cast<int>(e.bodySize));
                } else if constexpr (std::is_same_v<T, engine::ResponseReceived>) {
                    timeline_.onResponseReceived(
                        static_cast<int>(e.stepIndex),
                        e.status,
                        joinHeaders(e.headers),
                        static_cast<int>(e.bodySize),
                        static_cast<qint64>(e.elapsed.count()),
                        e.body ? QString::fromStdString(*e.body) : QString{});
                } else if constexpr (std::is_same_v<T, engine::ExtractionCompleted>) {
                    timeline_.onExtractionCompleted(static_cast<int>(e.stepIndex),
                                                    QString::fromStdString(e.op.value),
                                                    QString::fromStdString(e.variableName),
                                                    QString::fromStdString(e.sourcePath),
                                                    format::extractionOutcome(e.outcome),
                                                    QString::fromStdString(e.value));
                } else if constexpr (std::is_same_v<T, engine::AssertionCompleted>) {
                    timeline_.onAssertionCompleted(static_cast<int>(e.stepIndex),
                                                   QString::fromStdString(e.op.value),
                                                   QString::fromStdString(e.name),
                                                   QString::fromStdString(e.expr),
                                                   e.passed);
                } else if constexpr (std::is_same_v<T, engine::StepFailed>) {
                    timeline_.onStepFailed(static_cast<int>(e.stepIndex),
                                           QString::fromStdString(e.op.value),
                                           format::errorCode(e.code),
                                           QString::fromStdString(e.detail));
                } else if constexpr (std::is_same_v<T, engine::RunEnded>) {
                    timeline_.onRunEnded(format::runOutcome(e.outcome));
                }
            },
            event);
    }

    emit runReplayed();
}

QStringList AppController::operationIds() const {
    QStringList ids;
    if (activeProject().hasProject()) {
        for (const auto& [resId, resource] : activeProject().project().resources) {
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

void AppController::editOperationById(const QString& operationId) {
    selectOperationById(operationId);
    if (hasOperation_ && !editing_) {
        beginEdit();
    }
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
    if (activeProject().createResource(name, description, error)) {
        emit notify(QStringLiteral("Created module “%1”").arg(name.trimmed()), false);
    } else {
        emit notify(error, true);
    }
}

void AppController::createProject(const QUrl& directory, const QString& name) {
    const QString path = directory.isLocalFile() ? directory.toLocalFile() : directory.toString();
    if (path.isEmpty()) {
        emit notify(tr("Choose a folder for the new project."), true);
        return;
    }
    QString error;
    if (activeProject().createProject(path, name, error)) {
        const QString shown = name.trimmed().isEmpty() ? tr("project") : name.trimmed();
        emit notify(QStringLiteral("Created project “%1”").arg(shown), false);
    } else {
        emit notify(error, true);
    }
}

void AppController::prepareNewEndpoint(const QString& /*preselectedResource*/) {
    // Dependency candidates are every existing operation; the dialog filters
    // out self-reference by construction (the new op isn't created yet).
    newEndpointDeps_.setCandidates(operationIds());
    newEndpointExtractions_.clearRows();
    rebuildNewEndpointDepExtracts();
}

void AppController::rebuildNewEndpointDepExtracts() {
    std::vector<ChainEditorModel::OpSeed> seeds;
    if (activeProject().hasProject()) {
        for (const auto& depStd : newEndpointDeps_.dependencies()) {
            const QString id = QString::fromStdString(depStd);
            const auto* op = activeProject().findOperation(engine::OperationId{depStd});
            ChainEditorModel::OpSeed seed;
            seed.operationId = id;
            seed.method = op != nullptr ? methodLabel(op->method) : QString{};
            seed.isTarget = false;
            if (op != nullptr) {
                for (const auto& ext : op->extractions) {
                    seed.extractions.emplace_back(QString::fromStdString(ext.variableName),
                                                  QString::fromStdString(ext.sourcePath));
                }
            }
            seeds.push_back(std::move(seed));
        }
    }
    newEndpointDepExtracts_.rebuild(seeds);
}

void AppController::addNewEndpointDependency(const QString& operationId) {
    if (operationId.isEmpty()) {
        return;
    }
    // Setting the trailing blank (ghost) row appends and grows a new ghost.
    const int ghost = newEndpointDeps_.rowCount() - 1;
    newEndpointDeps_.setSelection(ghost >= 0 ? ghost : 0, operationId);
}

void AppController::removeNewEndpointDependency(const QString& operationId) {
    for (int row = 0; row < newEndpointDeps_.rowCount(); ++row) {
        const QString value =
            newEndpointDeps_.data(newEndpointDeps_.index(row, 0), DependencyEditModel::ValueRole)
                .toString();
        if (value == operationId) {
            newEndpointDeps_.removeRow(row);
            return;
        }
    }
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
    const auto created = activeProject().createOperation(engine::ResourceId{module.toStdString()},
                                                         name,
                                                         methodFromLabel(method),
                                                         path,
                                                         engine::ActorId{actor.toStdString()},
                                                         dependencies,
                                                         extractions,
                                                         error);
    if (!created) {
        emit notify(error, true);
        return;
    }

    // Persist any edits to the dependencies' own extract blocks (the "pull
    // X from this prerequisite" rows), so a value declared here is saved on
    // the producing endpoint where the engine reads it.
    std::map<std::string, engine::Operation> depUpdates;
    for (int i = 0; i < newEndpointDepExtracts_.count(); ++i) {
        const QString id = newEndpointDepExtracts_.operationIdAt(i);
        const auto* op = activeProject().findOperation(engine::OperationId{id.toStdString()});
        if (op == nullptr) {
            continue;
        }
        engine::Operation updated = *op;
        std::map<std::string, engine::Extraction::Source> sourceByVar;
        for (const auto& ext : op->extractions) {
            sourceByVar[ext.variableName] = ext.source;
        }
        updated.extractions.clear();
        for (const auto& [variable, sourcePath] :
             newEndpointDepExtracts_.extractModelAt(i)->pairs()) {
            const QString var = variable.trimmed();
            const QString p = sourcePath.trimmed();
            if (var.isEmpty() && p.isEmpty()) {
                continue;
            }
            engine::Extraction extraction;
            extraction.variableName = var.toStdString();
            extraction.sourcePath = p.toStdString();
            const auto found = sourceByVar.find(extraction.variableName);
            extraction.source =
                found != sourceByVar.end() ? found->second : sourceForPath(extraction.sourcePath);
            updated.extractions.push_back(std::move(extraction));
        }
        depUpdates.emplace(id.toStdString(), std::move(updated));
    }
    if (!depUpdates.empty()) {
        QString depError;
        if (!activeProject().saveOperations(depUpdates, depError)) {
            emit notify(depError, true);
        }
    }

    emit notify(QStringLiteral("Created endpoint “%1”").arg(QString::fromStdString(created->value)),
                false);
    selectOperationById(QString::fromStdString(created->value));
}

void AppController::renameOperation(const QString& operationId, const QString& newName) {
    QString error;
    if (activeProject().renameOperation(
            engine::OperationId{operationId.toStdString()}, newName, error)) {
        emit notify(QStringLiteral("Renamed endpoint"), false);
    } else {
        emit notify(error, true);
    }
}

void AppController::deleteOperation(const QString& operationId) {
    QString error;
    if (activeProject().deleteOperation(engine::OperationId{operationId.toStdString()}, error)) {
        emit notify(QStringLiteral("Deleted endpoint"), false);
    } else {
        emit notify(error, true);
    }
}

void AppController::renameResource(const QString& resourceId, const QString& newName) {
    QString error;
    if (activeProject().renameResource(
            engine::ResourceId{resourceId.toStdString()}, newName, error)) {
        emit notify(QStringLiteral("Renamed module"), false);
    } else {
        emit notify(error, true);
    }
}

void AppController::deleteResource(const QString& resourceId) {
    QString error;
    if (activeProject().deleteResource(engine::ResourceId{resourceId.toStdString()}, error)) {
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
        responseBody_.setBody(r.body);
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
