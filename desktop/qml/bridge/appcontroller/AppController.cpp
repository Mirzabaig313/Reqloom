// AppController — see header.
#include "AppController.h"

#include "ThemeController.h"
#include "application/EnvironmentSettings.h"
#include "application/OAuth2AuthCodeFlow.h"
#include "application/ProjectModel.h"
#include "application/WorkspaceModel.h"
#include "views/Formatting.h"
#include "views/HookEditorDialog.h"
#include "views/PathEval.h"
#include "widgets/GraphLayout.h"
#include "widgets/LineDiff.h"

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/FormBody.h>
#include <reqloom/engine/Predicate.h>

#include <QtConcurrent/QtConcurrentRun>

#include <QtCore/QByteArray>
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
#include <QtWidgets/QFileDialog>

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

#include "AppControllerInternal.h"
namespace reqloom::desktop::qml {

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

    // Toggle the active tab's dirty dot as its edit mode opens/closes.
    connect(this, &AppController::editingChanged, this, &AppController::updateActiveTabDirty);

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

QString AppController::explorerFilter() const {
    return treeFilter_.filterText();
}

void AppController::setExplorerFilter(const QString& text) {
    if (treeFilter_.filterText() == text) {
        return;
    }
    treeFilter_.setFilterText(text);
    emit explorerFilterChanged();
}

QStringList AppController::loadTreeExpansion() const {
    QSettings settings;
    return settings.value(QStringLiteral("explorer/expandedKeys")).toStringList();
}

void AppController::saveTreeExpansion(const QStringList& keys) const {
    QSettings settings;
    settings.setValue(QStringLiteral("explorer/expandedKeys"), keys);
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
    // Feed every open (loaded) collection to the aggregated explorer tree, along
    // with each collection's saved-example rows (read from its own store on
    // disk), so examples show for ALL open projects — not just the active one.
    // ProjectRootRole on every row lets a click resolve the owning project.
    std::vector<ProjectTreeModel::ProjectEntry> entries;
    QMap<QString, QList<ProjectTreeModel::ExampleRow>> examples;
    for (int i = 0; i < workspace_->count(); ++i) {
        const ProjectModel* p = workspace_->at(i);
        if (p == nullptr || !p->hasProject()) {
            continue;
        }
        const QString root = p->rootPath();
        ProjectTreeModel::ProjectEntry entry;
        entry.root = root;
        entry.name = p->name();
        entry.project = p->projectPtr();
        entry.active = i == workspace_->activeIndex();
        entries.push_back(std::move(entry));

        // Each collection persists its examples under its own root; read them
        // with a throwaway store so every project's rows appear in the tree.
        // synchronous per-project disk read on the GUI thread, run on
        // load/save/example-mutation (not on plain switch). Fine at expected
        // scale; move to QtConcurrent if workspaces grow to many large projects.
        SavedResponseStore store;
        store.setProjectRoot(root);
        for (const QString& id : store.operationIds()) {
            QList<ProjectTreeModel::ExampleRow> rows;
            for (const SavedResponse& r : store.list(id)) {
                rows.append(ProjectTreeModel::ExampleRow{r.name, r.status});
            }
            examples.insert(ProjectTreeModel::exampleKey(root, id), rows);
        }
    }
    tree_.populate(entries, examples);
}

void AppController::selectFirstModule() {
    // If the workspace restored open tabs for this project, don't auto-open the
    // first module on top of them — the restored active tab already shows.
    if (tabs_.count() > 0) {
        return;
    }
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

    // Tabs belong to the active collection (v1); a project switch/close starts
    // with a fresh, empty tab strip. The per-branch pane reset below clears the
    // live hasOperation_/hasActor_ flags.
    tabs_.clearAll();
    activeTabIndex_ = -1;
    emit activeTabChanged();

    // A read-only actor detail belongs to the previously-active collection.
    clearActorSelection();

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
    // Only refresh the open-op example panel here. The aggregated tree (with
    // every collection's examples) is rebuilt by populateWorkspaceTree() above
    // when repopulateTree is set; a plain project switch must NOT reset the
    // tree (it would collapse/re-expand the whole thing).
    refreshOpenOpExamples();
    // Re-open this project's saved editor tabs (per-project persistence). The
    // tab strip was cleared above; restore what was open last time this
    // collection was active (survives project switch-and-return + app restart).
    restoreOpenTabs(activeProject().rootPath());
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
    // capped at 15 entries — a flat rewrite is fine at this size; a
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
        // rejects `..` traversal. importAny sniffs the format and dispatches.
        auto imported = engine::importAny(spec, spec.parent_path());
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

    // Keep an open read-only actor detail in sync after an edit. If the actor
    // was renamed/deleted, its old id no longer exists → clear the panel.
    if (hasActor_) {
        const bool stillExists = activeProject().hasProject() &&
                                 activeProject().project().actors.count(
                                     engine::ActorId{selectedActorId_.toStdString()}) > 0;
        if (stillExists) {
            prepareEditActor(selectedActorId_);
            selectedActorDescription_ = actorDescription(selectedActorId_);
            selectedActorStrategy_ = actorAuthLabel(selectedActorId_);
            emit actorSelectionChanged();
        } else {
            clearActorSelection();
        }
    }

    const QString openModule = selectedModule_;
    const QString openOp = hasOperation_ ? opName_ : QString{};
    if (!openModule.isEmpty()) {
        const auto& resources = activeProject().project().resources;
        const auto it = resources.find(engine::ResourceId{openModule.toStdString()});
        if (it != resources.end()) {
            operations_.reload(it->second);
        }
    }
    // populateWorkspaceTree() above already rebuilt the tree (with examples);
    // just refresh the open op's example panel here.
    refreshOpenOpExamples();
    emit projectChanged();
    emit selectionChanged();

    // Refresh the open operation's read fields from the saved project, but only
    // when not editing (re-selecting would discard an in-progress edit).
    if (!editing_ && !openModule.isEmpty() && !openOp.isEmpty()) {
        const auto& resources = activeProject().project().resources;
        const auto it = resources.find(engine::ResourceId{openModule.toStdString()});
        if (it != resources.end() && it->second.operations.contains(openOp.toStdString())) {
            // Reload the active tab's read fields in place (no new tab).
            loadOperationReadState(openModule, openOp);
            captureActiveTab();
            emit operationChanged();
            emit chainChanged();
        }
    }
}

void AppController::selectActor(const QString& projectRoot, const QString& actorId) {
    if (!activateForRow(projectRoot)) {
        return;
    }
    if (!activeProject().hasProject() || actorId.isEmpty()) {
        return;
    }
    // Open (or focus) a tab for this actor; the per-tab buffer preserves its
    // in-progress edits alongside any open endpoint tabs.
    openActorTab(projectRoot, actorId, /*isNewDraft=*/false);
}

void AppController::clearActorSelection() {
    if (!hasActor_) {
        return;
    }
    hasActor_ = false;
    selectedActorId_.clear();
    emit actorSelectionChanged();
}

void AppController::selectModule(const QString& moduleName) {
    clearActorSelection();
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
    // Open (or focus) a tab for this operation in the active collection. The
    // per-tab buffer preserves each open endpoint's edit + response state.
    openOperationTab(activeProject().rootPath(), moduleName, opName);
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
    // Actor and inline auth are mutually exclusive — selecting an actor drops
    // the inline type (fields kept, ignored while type is "none").
    if (!editActor_.isEmpty()) {
        editAuthType_ = QStringLiteral("none");
    }
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

namespace {
// Map between the InlineAuthType enum and the QML-facing type string.
[[nodiscard]] QString inlineAuthTypeToQString(engine::InlineAuthType type) {
    switch (type) {
        case engine::InlineAuthType::Bearer:
            return QStringLiteral("bearer");
        case engine::InlineAuthType::Basic:
            return QStringLiteral("basic");
        case engine::InlineAuthType::ApiKey:
            return QStringLiteral("apikey");
        case engine::InlineAuthType::AwsSigV4:
            return QStringLiteral("aws_sigv4");
        case engine::InlineAuthType::OAuth1:
            return QStringLiteral("oauth1");
        case engine::InlineAuthType::OAuth2:
            return QStringLiteral("oauth2");
        case engine::InlineAuthType::Jwt:
            return QStringLiteral("jwt");
        case engine::InlineAuthType::Mtls:
            return QStringLiteral("mtls");
        case engine::InlineAuthType::Inherit:
            return QStringLiteral("inherit");
        case engine::InlineAuthType::None:
            break;
    }
    return QStringLiteral("none");
}

[[nodiscard]] engine::InlineAuthType inlineAuthTypeFromQString(const QString& type) {
    if (type == QStringLiteral("bearer")) {
        return engine::InlineAuthType::Bearer;
    }
    if (type == QStringLiteral("basic")) {
        return engine::InlineAuthType::Basic;
    }
    if (type == QStringLiteral("apikey")) {
        return engine::InlineAuthType::ApiKey;
    }
    if (type == QStringLiteral("aws_sigv4")) {
        return engine::InlineAuthType::AwsSigV4;
    }
    if (type == QStringLiteral("oauth1")) {
        return engine::InlineAuthType::OAuth1;
    }
    if (type == QStringLiteral("oauth2")) {
        return engine::InlineAuthType::OAuth2;
    }
    if (type == QStringLiteral("jwt")) {
        return engine::InlineAuthType::Jwt;
    }
    if (type == QStringLiteral("mtls")) {
        return engine::InlineAuthType::Mtls;
    }
    if (type == QStringLiteral("inherit")) {
        return engine::InlineAuthType::Inherit;
    }
    return engine::InlineAuthType::None;
}
}  // namespace

void AppController::setEditAuthType(const QString& type) {
    if (editAuthType_ == type) {
        return;
    }
    editAuthType_ = type;
    // Mutually exclusive with actor — selecting a real inline type clears the
    // actor so the two can never both be active.
    if (editAuthType_ != QStringLiteral("none")) {
        editActor_.clear();
    }
    emit editChanged();
}

void AppController::setEditAuthToken(const QString& token) {
    if (editAuthToken_ == token) {
        return;
    }
    editAuthToken_ = token;
    emit editChanged();
}

void AppController::setEditAuthUsername(const QString& username) {
    if (editAuthUsername_ == username) {
        return;
    }
    editAuthUsername_ = username;
    emit editChanged();
}

void AppController::setEditAuthPassword(const QString& password) {
    if (editAuthPassword_ == password) {
        return;
    }
    editAuthPassword_ = password;
    emit editChanged();
}

void AppController::setEditAuthApiKeyName(const QString& name) {
    if (editAuthApiKeyName_ == name) {
        return;
    }
    editAuthApiKeyName_ = name;
    emit editChanged();
}

void AppController::setEditAuthApiKeyValue(const QString& value) {
    if (editAuthApiKeyValue_ == value) {
        return;
    }
    editAuthApiKeyValue_ = value;
    emit editChanged();
}

void AppController::setEditAuthApiKeyInQuery(bool inQuery) {
    if (editAuthApiKeyInQuery_ == inQuery) {
        return;
    }
    editAuthApiKeyInQuery_ = inQuery;
    emit editChanged();
}

void AppController::setEditAuthAwsAccessKey(const QString& value) {
    if (editAuthAwsAccessKey_ == value) {
        return;
    }
    editAuthAwsAccessKey_ = value;
    emit editChanged();
}

void AppController::setEditAuthAwsSecretKey(const QString& value) {
    if (editAuthAwsSecretKey_ == value) {
        return;
    }
    editAuthAwsSecretKey_ = value;
    emit editChanged();
}

void AppController::setEditAuthAwsRegion(const QString& value) {
    if (editAuthAwsRegion_ == value) {
        return;
    }
    editAuthAwsRegion_ = value;
    emit editChanged();
}

void AppController::setEditAuthAwsService(const QString& value) {
    if (editAuthAwsService_ == value) {
        return;
    }
    editAuthAwsService_ = value;
    emit editChanged();
}

void AppController::setEditAuthAwsSessionToken(const QString& value) {
    if (editAuthAwsSessionToken_ == value) {
        return;
    }
    editAuthAwsSessionToken_ = value;
    emit editChanged();
}

void AppController::setEditAuthOauthConsumerKey(const QString& value) {
    if (editAuthOauthConsumerKey_ == value) {
        return;
    }
    editAuthOauthConsumerKey_ = value;
    emit editChanged();
}

void AppController::setEditAuthOauthConsumerSecret(const QString& value) {
    if (editAuthOauthConsumerSecret_ == value) {
        return;
    }
    editAuthOauthConsumerSecret_ = value;
    emit editChanged();
}

void AppController::setEditAuthOauthToken(const QString& value) {
    if (editAuthOauthToken_ == value) {
        return;
    }
    editAuthOauthToken_ = value;
    emit editChanged();
}

void AppController::setEditAuthOauthTokenSecret(const QString& value) {
    if (editAuthOauthTokenSecret_ == value) {
        return;
    }
    editAuthOauthTokenSecret_ = value;
    emit editChanged();
}

void AppController::setEditAuthOauth2GrantType(const QString& value) {
    if (editAuthOauth2GrantType_ == value) {
        return;
    }
    editAuthOauth2GrantType_ = value;
    emit editChanged();
}

void AppController::setEditAuthOauth2ClientAuth(const QString& value) {
    if (editAuthOauth2ClientAuth_ == value) {
        return;
    }
    editAuthOauth2ClientAuth_ = value;
    emit editChanged();
}

void AppController::setEditAuthOauth2TokenUrl(const QString& value) {
    if (editAuthOauth2TokenUrl_ == value) {
        return;
    }
    editAuthOauth2TokenUrl_ = value;
    emit editChanged();
}

void AppController::setEditAuthOauth2AuthUrl(const QString& value) {
    if (editAuthOauth2AuthUrl_ == value) {
        return;
    }
    editAuthOauth2AuthUrl_ = value;
    emit editChanged();
}

void AppController::setEditAuthOauth2CallbackUrl(const QString& value) {
    if (editAuthOauth2CallbackUrl_ == value) {
        return;
    }
    editAuthOauth2CallbackUrl_ = value;
    emit editChanged();
}

void AppController::setEditAuthOauth2PkceMethod(const QString& value) {
    if (editAuthOauth2PkceMethod_ == value) {
        return;
    }
    editAuthOauth2PkceMethod_ = value;
    emit editChanged();
}

void AppController::oauth2GetNewToken() {
    OAuth2AuthCodeFlow::Config config;
    config.authUrl = editAuthOauth2AuthUrl_;
    config.tokenUrl = editAuthOauth2TokenUrl_;
    config.clientId = editAuthOauth2ClientId_;
    config.clientSecret = editAuthOauth2ClientSecret_;
    config.scope = editAuthOauth2Scope_;
    config.callbackUrl = editAuthOauth2CallbackUrl_;
    config.clientAuth = editAuthOauth2ClientAuth_;
    config.pkceMethod = editAuthOauth2PkceMethod_;

    // One flow at a time; a fresh click supersedes any in-flight attempt.
    oauthFlow_ = std::make_unique<OAuth2AuthCodeFlow>();
    connect(oauthFlow_.get(), &OAuth2AuthCodeFlow::succeeded, this, [this](const QString& token) {
        editAuthOauth2AccessToken_ = token;
        emit editChanged();
        emit notify(QStringLiteral("Access token acquired"), false);
    });
    connect(oauthFlow_.get(), &OAuth2AuthCodeFlow::failed, this, [this](const QString& error) {
        emit notify(QStringLiteral("OAuth2 authorization failed: %1").arg(error), true);
    });
    emit notify(QStringLiteral("Opening browser to authorize…"), false);
    oauthFlow_->start(config);
}

void AppController::setEditAuthOauth2ClientId(const QString& value) {
    if (editAuthOauth2ClientId_ == value) {
        return;
    }
    editAuthOauth2ClientId_ = value;
    emit editChanged();
}

void AppController::setEditAuthOauth2ClientSecret(const QString& value) {
    if (editAuthOauth2ClientSecret_ == value) {
        return;
    }
    editAuthOauth2ClientSecret_ = value;
    emit editChanged();
}

void AppController::setEditAuthOauth2Scope(const QString& value) {
    if (editAuthOauth2Scope_ == value) {
        return;
    }
    editAuthOauth2Scope_ = value;
    emit editChanged();
}

void AppController::setEditAuthJwtAlgorithm(const QString& value) {
    if (editAuthJwtAlgorithm_ == value) {
        return;
    }
    editAuthJwtAlgorithm_ = value;
    emit editChanged();
}

void AppController::setEditAuthJwtSecret(const QString& value) {
    if (editAuthJwtSecret_ == value) {
        return;
    }
    editAuthJwtSecret_ = value;
    emit editChanged();
}

void AppController::setEditAuthJwtPayload(const QString& value) {
    if (editAuthJwtPayload_ == value) {
        return;
    }
    editAuthJwtPayload_ = value;
    emit editChanged();
}

void AppController::setEditAuthMtlsCertPath(const QString& value) {
    if (editAuthMtlsCertPath_ == value) {
        return;
    }
    editAuthMtlsCertPath_ = value;
    emit editChanged();
}

void AppController::setEditAuthMtlsKeyPath(const QString& value) {
    if (editAuthMtlsKeyPath_ == value) {
        return;
    }
    editAuthMtlsKeyPath_ = value;
    emit editChanged();
}

void AppController::setEditAuthMtlsKeyPassword(const QString& value) {
    if (editAuthMtlsKeyPassword_ == value) {
        return;
    }
    editAuthMtlsKeyPassword_ = value;
    emit editChanged();
}

void AppController::setEditAuthMtlsFormat(const QString& value) {
    if (editAuthMtlsFormat_ == value) {
        return;
    }
    editAuthMtlsFormat_ = value;
    emit editChanged();
}

void AppController::setEditAuthMtlsCaCertPath(const QString& value) {
    if (editAuthMtlsCaCertPath_ == value) {
        return;
    }
    editAuthMtlsCaCertPath_ = value;
    emit editChanged();
}

QString AppController::pickFile(const QString& title, const QString& nameFilter) const {
    return QFileDialog::getOpenFileName(nullptr, title, QString{}, nameFilter);
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

    // Inline (actor-less) auth: seed from the op, or reset to "none".
    if (op->inlineAuth) {
        const auto& a = *op->inlineAuth;
        editAuthType_ = inlineAuthTypeToQString(a.type);
        editAuthToken_ = QString::fromStdString(a.token);
        editAuthUsername_ = QString::fromStdString(a.username);
        editAuthPassword_ = QString::fromStdString(a.password);
        editAuthApiKeyName_ = QString::fromStdString(a.apiKeyName);
        editAuthApiKeyValue_ = QString::fromStdString(a.apiKeyValue);
        editAuthApiKeyInQuery_ = a.apiKeyInQuery;
        editAuthAwsAccessKey_ = QString::fromStdString(a.awsAccessKey);
        editAuthAwsSecretKey_ = QString::fromStdString(a.awsSecretKey);
        editAuthAwsRegion_ = QString::fromStdString(a.awsRegion);
        editAuthAwsService_ = QString::fromStdString(a.awsService);
        editAuthAwsSessionToken_ = QString::fromStdString(a.awsSessionToken);
        editAuthOauthConsumerKey_ = QString::fromStdString(a.oauthConsumerKey);
        editAuthOauthConsumerSecret_ = QString::fromStdString(a.oauthConsumerSecret);
        editAuthOauthToken_ = QString::fromStdString(a.oauthToken);
        editAuthOauthTokenSecret_ = QString::fromStdString(a.oauthTokenSecret);
        // Clamp to the known options so the combo and the stored value never
        // diverge (an unknown value from hand-edited YAML falls back to default,
        // which is also how the engine interprets it).
        editAuthOauth2GrantType_ = a.oauth2GrantType == "password"
                                       ? QStringLiteral("password")
                                       : QStringLiteral("client_credentials");
        editAuthOauth2ClientAuth_ = a.oauth2ClientAuth == "basic"  ? QStringLiteral("basic")
                                    : a.oauth2ClientAuth == "none" ? QStringLiteral("none")
                                                                   : QStringLiteral("body");
        editAuthOauth2TokenUrl_ = QString::fromStdString(a.oauth2TokenUrl);
        editAuthOauth2ClientId_ = QString::fromStdString(a.oauth2ClientId);
        editAuthOauth2ClientSecret_ = QString::fromStdString(a.oauth2ClientSecret);
        editAuthOauth2Scope_ = QString::fromStdString(a.oauth2Scope);
        editAuthOauth2AuthUrl_ = QString::fromStdString(a.oauth2AuthUrl);
        editAuthOauth2CallbackUrl_ = a.oauth2CallbackUrl.empty()
                                         ? QStringLiteral("http://127.0.0.1:8080/callback")
                                         : QString::fromStdString(a.oauth2CallbackUrl);
        editAuthOauth2PkceMethod_ =
            a.oauth2PkceMethod == "plain" ? QStringLiteral("plain") : QStringLiteral("S256");
        // Token is ephemeral (never persisted); always starts empty on edit.
        editAuthOauth2AccessToken_.clear();
        editAuthJwtAlgorithm_ = a.jwtAlgorithm.empty() ? QStringLiteral("HS256")
                                                       : QString::fromStdString(a.jwtAlgorithm);
        editAuthJwtSecret_ = QString::fromStdString(a.jwtSecret);
        editAuthJwtPayload_ = QString::fromStdString(a.jwtPayload);
        editAuthMtlsFormat_ = a.mtlsFormat == "p12" ? QStringLiteral("p12") : QStringLiteral("pem");
        editAuthMtlsCertPath_ = QString::fromStdString(a.mtlsCertPath);
        editAuthMtlsKeyPath_ = QString::fromStdString(a.mtlsKeyPath);
        editAuthMtlsKeyPassword_ = QString::fromStdString(a.mtlsKeyPassword);
        editAuthMtlsCaCertPath_ = QString::fromStdString(a.mtlsCaCertPath);
    } else {
        editAuthType_ = QStringLiteral("none");
        editAuthToken_.clear();
        editAuthUsername_.clear();
        editAuthPassword_.clear();
        editAuthApiKeyName_.clear();
        editAuthApiKeyValue_.clear();
        editAuthApiKeyInQuery_ = false;
        editAuthAwsAccessKey_.clear();
        editAuthAwsSecretKey_.clear();
        editAuthAwsRegion_.clear();
        editAuthAwsService_.clear();
        editAuthAwsSessionToken_.clear();
        editAuthOauthConsumerKey_.clear();
        editAuthOauthConsumerSecret_.clear();
        editAuthOauthToken_.clear();
        editAuthOauthTokenSecret_.clear();
        editAuthOauth2GrantType_ = QStringLiteral("client_credentials");
        editAuthOauth2ClientAuth_ = QStringLiteral("basic");
        editAuthOauth2TokenUrl_.clear();
        editAuthOauth2ClientId_.clear();
        editAuthOauth2ClientSecret_.clear();
        editAuthOauth2Scope_.clear();
        editAuthOauth2AuthUrl_.clear();
        editAuthOauth2CallbackUrl_ = QStringLiteral("http://127.0.0.1:8080/callback");
        editAuthOauth2PkceMethod_ = QStringLiteral("S256");
        editAuthOauth2AccessToken_.clear();
        editAuthJwtAlgorithm_ = QStringLiteral("HS256");
        editAuthJwtSecret_.clear();
        editAuthJwtPayload_.clear();
        editAuthMtlsFormat_ = QStringLiteral("pem");
        editAuthMtlsCertPath_.clear();
        editAuthMtlsKeyPath_.clear();
        editAuthMtlsKeyPassword_.clear();
        editAuthMtlsCaCertPath_.clear();
    }

    // Actor and inline auth are mutually exclusive in the editor. If a
    // hand-edited op carries both, inline auth wins (it's applied last at
    // runtime), so drop the actor from the editor seed to match.
    if (editAuthType_ != QStringLiteral("none")) {
        editActor_.clear();
    }

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

std::optional<engine::InlineAuth> AppController::buildInlineAuthFromEdit() const {
    const auto authType = inlineAuthTypeFromQString(editAuthType_);
    if (authType == engine::InlineAuthType::None) {
        return std::nullopt;
    }
    engine::InlineAuth auth;
    auth.type = authType;
    auth.token = editAuthToken_.toStdString();
    auth.username = editAuthUsername_.toStdString();
    auth.password = editAuthPassword_.toStdString();
    auth.apiKeyName = editAuthApiKeyName_.toStdString();
    auth.apiKeyValue = editAuthApiKeyValue_.toStdString();
    auth.apiKeyInQuery = editAuthApiKeyInQuery_;
    auth.awsAccessKey = editAuthAwsAccessKey_.toStdString();
    auth.awsSecretKey = editAuthAwsSecretKey_.toStdString();
    auth.awsRegion = editAuthAwsRegion_.toStdString();
    auth.awsService = editAuthAwsService_.toStdString();
    auth.awsSessionToken = editAuthAwsSessionToken_.toStdString();
    auth.oauthConsumerKey = editAuthOauthConsumerKey_.toStdString();
    auth.oauthConsumerSecret = editAuthOauthConsumerSecret_.toStdString();
    auth.oauthToken = editAuthOauthToken_.toStdString();
    auth.oauthTokenSecret = editAuthOauthTokenSecret_.toStdString();
    auth.oauth2GrantType = editAuthOauth2GrantType_.toStdString();
    auth.oauth2ClientAuth = editAuthOauth2ClientAuth_.toStdString();
    auth.oauth2TokenUrl = editAuthOauth2TokenUrl_.toStdString();
    auth.oauth2ClientId = editAuthOauth2ClientId_.toStdString();
    auth.oauth2ClientSecret = editAuthOauth2ClientSecret_.toStdString();
    auth.oauth2Scope = editAuthOauth2Scope_.toStdString();
    auth.oauth2AuthUrl = editAuthOauth2AuthUrl_.toStdString();
    auth.oauth2CallbackUrl = editAuthOauth2CallbackUrl_.toStdString();
    auth.oauth2PkceMethod = editAuthOauth2PkceMethod_.toStdString();
    auth.oauth2AccessToken = editAuthOauth2AccessToken_.toStdString();
    auth.jwtAlgorithm = editAuthJwtAlgorithm_.toStdString();
    auth.jwtSecret = editAuthJwtSecret_.toStdString();
    auth.jwtPayload = editAuthJwtPayload_.toStdString();
    auth.mtlsFormat = editAuthMtlsFormat_.toStdString();
    auth.mtlsCertPath = editAuthMtlsCertPath_.toStdString();
    auth.mtlsKeyPath = editAuthMtlsKeyPath_.toStdString();
    auth.mtlsKeyPassword = editAuthMtlsKeyPassword_.toStdString();
    auth.mtlsCaCertPath = editAuthMtlsCaCertPath_.toStdString();
    return auth;
}

void AppController::saveProjectDefaultAuth() {
    if (!activeProject().hasProject()) {
        emit notify(QStringLiteral("Open a project first"), true);
        return;
    }
    const auto auth = buildInlineAuthFromEdit();
    if (!auth || auth->type == engine::InlineAuthType::Inherit) {
        emit notify(
            QStringLiteral("Pick a concrete Auth Type before saving it as the project default"),
            true);
        return;
    }
    QString error;
    if (activeProject().saveProjectDefaultAuth(auth, error)) {
        emit notify(QStringLiteral("Saved project default auth (%1)").arg(editAuthType_), false);
    } else {
        emit notify(error, true);
    }
}

QString AppController::projectDefaultAuthLabel() const {
    if (!activeProject().hasProject() || !activeProject().project().defaultAuth) {
        return QStringLiteral("(none set)");
    }
    return inlineAuthTypeToQString(activeProject().project().defaultAuth->type);
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

    // Inline auth: build the typed credential from the edit fields. "none"
    // leaves it nullopt so applyOverrideToOperation clears any prior auth.
    ov.inlineAuth = buildInlineAuthFromEdit();

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
        // The tab is already open; reload its read fields from the just-saved
        // project and re-snapshot the tab (don't spawn a duplicate tab).
        editing_ = false;
        loadOperationReadState(selectedModule_, opName_);
        captureActiveTab();
        emit editingChanged();
        emit operationChanged();
        emit chainChanged();
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
    // An example was added/renamed/deleted: rebuild the tree (which re-reads
    // every collection's examples) and refresh the open op's example list.
    // Resets the tree model, so callers only use this on load / example
    // mutation — never on a plain project switch (see rebindActiveProject).
    populateWorkspaceTree();
    refreshOpenOpExamples();
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
        case engine::AuthStrategy::Bearer:
            return QStringLiteral("Bearer Token");
        case engine::AuthStrategy::Jwt:
            return QStringLiteral("JWT Bearer");
        case engine::AuthStrategy::Mtls:
            return QStringLiteral("mTLS (Client Cert)");
    }
    return QStringLiteral("Custom");
}

QVariantList AppController::autoGeneratedHeaders() const {
    // The headers libcurl sends for us (see engine::HttpDefaults + CurlHttpClient).
    // Host is derived from the URL at send time, so it has no fixed value here.
    const auto sv = [](std::string_view s) {
        return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
    };
    const auto row = [](const QString& name, const QString& value) {
        return QVariantMap{{QStringLiteral("name"), name}, {QStringLiteral("value"), value}};
    };
    return QVariantList{
        row(QStringLiteral("Host"), tr("<calculated when request is sent>")),
        row(QStringLiteral("User-Agent"), sv(engine::kDefaultUserAgent)),
        row(QStringLiteral("Accept"), sv(engine::kDefaultAccept)),
        row(QStringLiteral("Accept-Encoding"), sv(engine::kDefaultAcceptEncoding)),
        row(QStringLiteral("Connection"), sv(engine::kDefaultConnection)),
    };
}

}  // namespace reqloom::desktop::qml
