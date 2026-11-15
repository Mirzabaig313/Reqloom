// AppController — see header.
#include "AppController.h"

#include "ThemeController.h"
#include "application/EditBody.h"
#include "application/EnvironmentSettings.h"
#include "application/OAuth2AuthCodeFlow.h"
#include "application/ProjectModel.h"
#include "application/WorkspaceModel.h"
#include "views/Formatting.h"
#include "views/HookEditorDialog.h"
#include "views/PathEval.h"
#include "widgets/BodyText.h"
#include "widgets/GraphLayout.h"
#include "widgets/LineDiff.h"

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/FormBody.h>
#include <reqloom/engine/ImportCredentials.h>
#include <reqloom/engine/Predicate.h>
#include <reqloom/engine/ProjectTrustStore.h>
#include <reqloom/engine/SecretStore.h>

#include <QtConcurrent/QtConcurrentRun>

#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QHash>
#include <QtCore/QRegularExpression>
#include <QtCore/QScopedValueRollback>
#include <QtCore/QSet>
#include <QtCore/QStandardPaths>
#include <QtCore/QStringList>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QDialog>
#include <QtWidgets/QFileDialog>

#include <algorithm>
#include <cstddef>
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

namespace {

/// How often a live session's remaining TTL is re-read for the explorer dot.
constexpr int kSessionTickMs = 15'000;

/// Engine session state → the token the explorer's dot switches on.
[[nodiscard]] QString sessionStateToken(engine::ActorSession::State state) {
    switch (state) {
        case engine::ActorSession::State::Live:
            return QStringLiteral("live");
        case engine::ActorSession::State::Authenticating:
            return QStringLiteral("authenticating");
        case engine::ActorSession::State::Refreshing:
            return QStringLiteral("refreshing");
        case engine::ActorSession::State::None:
            break;
    }
    return QStringLiteral("none");
}

}  // namespace

AppController::AppController(QObject* parent)
    : QObject(parent),
      workspace_(std::make_unique<WorkspaceModel>()),
      bootstrapper_(std::make_unique<Bootstrapper>()),
      runController_(
          std::make_unique<RunController>(bootstrapper_->engine(), *workspace_->active(), this)) {
    bindProject(workspace_->active());

    exampleStore_.setErrorHandler([this](const QString& message) { emit notify(message, true); });

    // Deliver the off-thread import result on the GUI thread, where touching models and emitting
    // signals is safe.
    connect(&importWatcher_, &QFutureWatcher<ImportOutcome>::finished, this, [this]() {
        const ImportOutcome out = importWatcher_.result();
        switch (out.status) {
            case ImportOutcome::Status::ImportFailed:
                emit notify(tr("Import failed: %1").arg(out.errorDetail), true);
                return;
            case ImportOutcome::Status::WriteFailed:
                emit notify(tr("Could not write project: %1").arg(out.errorDetail), true);
                return;
            case ImportOutcome::Status::Refused:
                emit notify(out.errorDetail, true);
                return;
            case ImportOutcome::Status::NeedsOverwrite:
                // Target folder exists: the UI confirms, then re-invokes importOpenApi(...,
                // overwrite=true).
                emit importNeedsOverwrite(QUrl::fromLocalFile(out.specPath),
                                          QUrl::fromLocalFile(out.baseDir));
                return;
            case ImportOutcome::Status::Success:
                break;
        }
        finishImport(out);
    });

    // Capture response bodies by default so the timeline shows full detail; RunController defaults
    // to off.
    runController_->setCaptureResponseBodies(captureBodies_);

    wireRunEvents();

    sessionTicker_.setInterval(kSessionTickMs);
    connect(&sessionTicker_, &QTimer::timeout, this, &AppController::refreshActorSessions);

    connect(this, &AppController::environmentChanged, this, [this]() {
        runController_->setEnvironment(environment_);
        refreshActorSessions();
        emit cookiesChanged();
    });

    treeFilter_.setSourceModel(&tree_);

    // Any edit-model change re-evaluates tab counts and the banner; the dependency model also
    // re-renders the chain preview.
    const auto onEditModelChanged = [this]() {
        emit editChanged();
        emit chainChanged();
    };
    for (QAbstractItemModel* model : {static_cast<QAbstractItemModel*>(&editHeaders_),
                                      static_cast<QAbstractItemModel*>(&editForm_),
                                      static_cast<QAbstractItemModel*>(&editExtractions_),
                                      static_cast<QAbstractItemModel*>(&editAssertions_)}) {
        connect(model, &QAbstractItemModel::dataChanged, this, onEditModelChanged);
        connect(model, &QAbstractItemModel::rowsInserted, this, onEditModelChanged);
        connect(model, &QAbstractItemModel::rowsRemoved, this, onEditModelChanged);
        connect(model, &QAbstractItemModel::modelReset, this, onEditModelChanged);
    }
    const auto applyQueryPath = [this](QString path) {
        if (path == editPath_) {
            return;
        }
        QScopedValueRollback<int> querySync{querySyncDepth_, querySyncDepth_ + 1};
        editPath_ = std::move(path);
        emit editChanged();
        emit chainChanged();
    };
    connect(&editQuery_,
            &QAbstractItemModel::dataChanged,
            this,
            [this, applyQueryPath](const QModelIndex& topLeft) {
                if (querySyncDepth_ > 0 || !topLeft.isValid()) {
                    return;
                }
                const std::pair<QString, QString> pair{
                    editQuery_.data(topLeft, EditableKeyValueModel::KeyRole).toString(),
                    editQuery_.data(topLeft, EditableKeyValueModel::ValueRole).toString()};
                applyQueryPath(visiblePathWithUpdatedQueryPair(
                    editPath_, static_cast<std::size_t>(topLeft.row()), pair));
            });
    connect(&editQuery_,
            &QAbstractItemModel::rowsRemoved,
            this,
            [this, applyQueryPath](const QModelIndex&, int first, int last) {
                if (querySyncDepth_ > 0) {
                    return;
                }
                applyQueryPath(
                    visiblePathWithoutQueryPairs(editPath_,
                                                 static_cast<std::size_t>(first),
                                                 static_cast<std::size_t>(last - first + 1)));
            });
    const auto onDepsChanged = [this]() {
        emit editChanged();
        emit chainChanged();
    };
    connect(&editDependencies_, &QAbstractItemModel::dataChanged, this, onDepsChanged);
    connect(&editDependencies_, &QAbstractItemModel::rowsInserted, this, onDepsChanged);

    connect(&editDependencies_, &QAbstractItemModel::rowsRemoved, this, onDepsChanged);
    connect(&editDependencies_, &QAbstractItemModel::modelReset, this, onDepsChanged);
    connect(&chainEditor_, &ChainEditorModel::editorChanged, this, onDepsChanged);

    wireTabTracking();

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
    // UniqueConnection so re-binding the same project never stacks connections.
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
    // Canonicalize so dedup and persistence use the key ProjectModel::rootPath() reports.
    std::error_code ec;
    const std::filesystem::path canon =
        std::filesystem::weakly_canonical(std::filesystem::path{path.toStdString()}, ec);
    const QString canonical = ec ? path : QString::fromStdString(canon.string());

    // Already open: activate only (a no-op for the current project, so an open draft does not block
    // it).
    if (const int existing = workspace_->indexOfRoot(canonical); existing >= 0) {
        activateProject(existing);
        return;
    }
    if (runController_->isRunning()) {
        emit notify(tr("Finish the current run before opening another project."), true);
        return;
    }
    if (!canLeaveActiveProject([this, canonical]() { openProjectPath(canonical); })) {
        return;
    }

    // Reuse the current slot when it is an empty sentinel; otherwise add and activate a collection
    // first so the synchronous `loaded` rebinds to the right project.
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
    QString failure;
    const auto captureFailure = connect(
        target, &ProjectModel::loadFailed, this, [&failure](const QString&, const QString& d) {
            failure = d;
        });
    target->loadFromDirectory(canonical);
    disconnect(captureFailure);

    if (target->hasProject()) {
        const bool forgotten = workspace_->forgetUnavailable(path);
        if (workspace_->forgetUnavailable(canonical) || forgotten) {
            persistOpenProjects();
        }
        return;
    }
    for (const QString& key : {path, canonical}) {
        if (!failure.isEmpty() && workspace_->isUnavailable(key)) {
            workspace_->markUnavailable(key, QString{}, failure);
            emit openProjectsChanged();
        }
    }
    if (!reuse) {
        // Failed load on a fresh slot: drop it and rebind to the previously active project.
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
    if (index == workspace_->activeIndex()) {
        return;
    }
    if (runController_->isRunning()) {
        emit notify(tr("Finish the current run before switching projects."), true);
        return;
    }
    if (!canLeaveActiveProject([this, index]() { activateProject(index); })) {
        return;
    }
    workspace_->setActiveIndex(index);
    persistActiveProject();
    // The tree already lists every collection; just rebind and show the first module.
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
    if (wasActive && !canLeaveActiveProject([this, index]() { closeProject(index); })) {
        return;
    }
    if (const ProjectModel* closing = workspace_->at(index); closing != nullptr) {
        runController_->forgetProject(closing->rootPath());
    }
    workspace_->removeProject(index);
    persistOpenProjects();
    persistActiveProject();
    if (wasActive) {
        rebindActiveProject(/*repopulateTree=*/true);
        selectFirstModule();
    } else {
        populateWorkspaceTree();
        emit openProjectsChanged();
    }
}

void AppController::populateWorkspaceTree() {
    // Feed every open collection and its saved examples to the aggregated tree; ProjectRootRole
    // lets a click resolve the owning project.
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

        // Synchronous GUI-thread read of the examples database; fine at expected scale.
        SavedResponseStore store;
        store.setProjectRoot(root, QString::fromStdString(p->project().id));
        for (const QString& id : store.operationIds()) {
            QList<ProjectTreeModel::ExampleRow> rows;
            for (const SavedResponse& r : store.list(id)) {
                rows.append(ProjectTreeModel::ExampleRow{r.name, r.status});
            }
            examples.insert(ProjectTreeModel::exampleKey(root, id), rows);
        }
    }
    tree_.populate(entries, examples);
    // populate() resets the tree, so re-apply the session snapshot.
    refreshActorSessions();
}

void AppController::selectFirstModule() {
    // Restored tabs already show the active tab; don't auto-open the first module over them.
    if (tabs_.count() > 0) {
        return;
    }
    if (activeProject().hasProject() && !activeProject().project().resources.empty()) {
        selectModule(
            QString::fromStdString(activeProject().project().resources.begin()->first.value));
    }
}

void AppController::rebindActiveProject(bool repopulateTree) {
    // Idempotent (UniqueConnection); guarantees even a fresh sentinel is bound before the next
    // load.
    bindProject(workspace_->active());

    // Point the run controller at the active project first; every run and cookie query resolves
    // against it.
    runController_->setProject(activeProject());

    // Tabs belong to the active collection; a switch or close starts with an empty tab strip.
    const bool droppedDraft = operationDraftIndex() >= 0;
    tabs_.clearAll();
    activeTabIndex_ = -1;
    if (droppedDraft) {
        clearNewOperationDraftIdentity();
        emit newOperationDraftDiscarded();
    }
    emit activeTabChanged();

    clearActorSelection();

    if (repopulateTree) {
        populateWorkspaceTree();
    }

    if (!activeProject().hasProject()) {
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
    openExampleStore();
    // Per-project run-history database, keyed by a hash of the project root.
    openProjectHistory(activeProject().rootPath());
    environments_ = activeProject().environmentNames();
    {
        // Each project restores its own selection; the previous project's says nothing about it.
        QSettings settings;
        const QString chosen =
            chooseEnvironment(environments_,
                              EnvironmentSettings::load(settings, activeProject().rootPath()),
                              activeProject().defaultEnvironment());
        if (chosen != environment_) {
            environment_ = chosen;
            emit environmentChanged();
        }
    }
    // The previously open op belonged to the prior project; callers select what comes next.
    selectedModule_.clear();
    operations_.reset();
    closeOperation();
    // The aggregated tree is rebuilt by populateWorkspaceTree() when repopulateTree is set;
    // a plain switch must not reset it (it would collapse the whole tree).
    refreshOpenOpExamples();
    // Restore this project's saved editor tabs (the strip was cleared above).
    restoreOpenTabs(activeProject().rootPath());
    emit projectChanged();
    emit selectionChanged();
    emit openProjectsChanged();
}

bool AppController::activateForRow(const QString& projectRoot, std::function<void()> retry) {
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
    if (!retry) {
        retry = [this, projectRoot]() {
            (void)activateForRow(projectRoot);
        };
    }
    if (!canLeaveActiveProject(retry)) {
        return false;
    }
    workspace_->setActiveIndex(idx);
    persistActiveProject();
    // No tree repopulate and no auto-select: the caller opens a specific operation.
    rebindActiveProject(/*repopulateTree=*/false);
    return true;
}

void AppController::selectOperationInProject(const QString& projectRoot,
                                             const QString& operationId) {
    if (!activateForRow(projectRoot, [this, projectRoot, operationId]() {
            selectOperationInProject(projectRoot, operationId);
        })) {
        return;
    }
    selectOperationById(operationId);
}

void AppController::activateOperationInProject(const QString& projectRoot,
                                               const QString& operationId) {
    if (!activateForRow(projectRoot, [this, projectRoot, operationId]() {
            activateOperationInProject(projectRoot, operationId);
        })) {
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
        // Normalize trailing slashes so the same project does not appear twice.
        const QString path =
            canonicalProjectPath(settings.value(QStringLiteral("path")).toString());
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
// Persist the list, clearing prior entries first so a shrunk list leaves no stale indices.
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
    // Cap on recent entries; a flat rewrite is fine at this size.
    constexpr int kMaxRecent = 15;
    // Normalize so a differently-spelled path doesn't create a second recent entry.
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

QVariantList AppController::unavailableProjects() const {
    QVariantList out;
    for (const WorkspaceModel::UnavailableProject& entry : workspace_->unavailable()) {
        out.append(QVariantMap{{QStringLiteral("path"), entry.path},
                               {QStringLiteral("name"), entry.name},
                               {QStringLiteral("reason"), entry.reason}});
    }
    return out;
}

void AppController::retryUnavailableProject(const QString& path) {
    if (!workspace_->isUnavailable(path)) {
        return;
    }
    if (!QFileInfo::exists(path + QStringLiteral("/reqloom.yaml"))) {
        workspace_->markUnavailable(
            path, QString{}, tr("Folder or reqloom.yaml not found: %1").arg(path));
        emit openProjectsChanged();
        emit notify(tr("Still unavailable: %1").arg(path), true);
        return;
    }
    openProjectPath(path);
}

void AppController::removeUnavailableProject(const QString& path) {
    if (workspace_->forgetUnavailable(path)) {
        persistOpenProjects();
    }
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
    for (const WorkspaceModel::SavedProject& saved : workspace_->savedProjects()) {
        settings.setArrayIndex(written++);
        settings.setValue(QStringLiteral("name"), saved.name);
        settings.setValue(QStringLiteral("path"), saved.path);
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
    QList<WorkspaceModel::SavedProject> saved;
    QString activePath;
    {
        QSettings settings;
        const int count = settings.beginReadArray(QStringLiteral("workspace/openProjects"));
        for (int i = 0; i < count; ++i) {
            settings.setArrayIndex(i);
            const QString p = settings.value(QStringLiteral("path")).toString();
            if (!p.isEmpty()) {
                saved.append(
                    {.path = p, .name = settings.value(QStringLiteral("name")).toString()});
            }
        }
        settings.endArray();
        activePath = settings.value(QStringLiteral("workspace/activeProject")).toString();
    }

    if (saved.isEmpty()) {
        // No saved workspace: fall back to the dev sample so a first run still opens something.
        loadSampleIfPresent();
        return;
    }

    // A saved project that cannot be opened (unmounted drive, load error) stays listed as
    // unavailable until the user removes it, so a restart never forgets it.
    for (const WorkspaceModel::SavedProject& entry : saved) {
        const bool exists = QFileInfo::exists(entry.path + QStringLiteral("/reqloom.yaml"));
        workspace_->markUnavailable(
            entry.path,
            entry.name,
            exists ? tr("Could not be opened.")
                   : tr("Folder or reqloom.yaml not found: %1").arg(entry.path));
    }
    for (const WorkspaceModel::SavedProject& entry : saved) {
        if (QFileInfo::exists(entry.path + QStringLiteral("/reqloom.yaml"))) {
            openProjectPath(entry.path);
        }
        if (workspace_->indexOfRoot(entry.path) >= 0) {
            workspace_->forgetUnavailable(entry.path);
        }
    }
    persistOpenProjects();
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
    // Destination is optional (default: next to the source file); a named sub-folder is always
    // created so reqloom.yaml never lands in the base directory. `targetDir` is only supplied when
    // overwriting.
    const QString dirPath =
        targetDir.isLocalFile() ? targetDir.toLocalFile() : targetDir.toString();

    ImportRequest request;
    request.spec = std::filesystem::path{specPath.toStdString()};
    if (!dirPath.isEmpty()) {
        request.baseDir = std::filesystem::path{dirPath.toStdString()};
    }
    request.overwrite = overwrite;
    request.blocked = importBlockedFolders();
    importWatcher_.setFuture(QtConcurrent::run(
        [request = std::move(request)]() { return runImportJobWithUserStores(request); }));
}

std::vector<BlockedFolder> AppController::importBlockedFolders() const {
    const bool running = runController_->isRunning();
    bool activeUnsaved = operationDraftIndex() >= 0;
    for (int i = 0; i < tabs_.count() && !activeUnsaved; ++i) {
        activeUnsaved = tabs_.stateAt(i).dirty;
    }
    std::vector<BlockedFolder> blocked;
    for (int i = 0; i < workspace_->count(); ++i) {
        const ProjectModel* project = workspace_->at(i);
        if (project == nullptr || !project->hasProject()) {
            continue;
        }
        // Reloading needs the project idle; a run holds every project in place.
        QString reason;
        if (running) {
            reason = tr("a run is in progress");
        } else if (i == workspace_->activeIndex() && activeUnsaved) {
            reason = tr("has unsaved edits");
        }
        if (!reason.isEmpty()) {
            blocked.push_back({std::filesystem::path{project->rootPath().toStdString()}, reason});
        }
    }
    return blocked;
}

void AppController::finishImport(const ImportOutcome& outcome) {
    const QString root = canonicalProjectPath(outcome.loadDir);
    const QString shown = QDir::toNativeSeparators(outcome.loadDir);
    const int open = workspace_->indexOfRoot(root);
    if (open != workspace_->activeIndex()) {
        if (runController_->isRunning()) {
            emit notify(tr("Imported into %1. Finish the current run, then open it.").arg(shown),
                        true);
            return;
        }
        if (!canLeaveActiveProject([this, outcome]() { finishImport(outcome); })) {
            return;
        }
    }
    // A run or an edit may have started while the import ran.
    for (const BlockedFolder& blocked : importBlockedFolders()) {
        if (canonicalProjectPath(QString::fromStdString(blocked.root.string())) == root) {
            emit notify(tr("Imported into %1. The open project there was not reloaded (%2); "
                           "reopen it to see the import.")
                            .arg(shown, blocked.reason),
                        true);
            return;
        }
    }

    bool loaded = false;
    if (open >= 0) {
        activateProject(open);
        if (workspace_->activeIndex() == open) {
            ProjectModel& project = activeProject();
            const auto before = project.projectPtr();
            project.loadFromDirectory(project.rootPath());
            loaded = project.projectPtr() != before;
        }
    } else {
        openProjectPath(outcome.loadDir);
        const int now = workspace_->indexOfRoot(root);
        loaded = now >= 0 && now == workspace_->activeIndex() && activeProject().hasProject();
    }
    if (!loaded) {
        emit notify(tr("Imported into %1, but the project could not be opened.").arg(shown), true);
        return;
    }

    QString summary =
        tr("Imported %1 resources, %2 operations.").arg(outcome.resources).arg(outcome.operations);
    if (outcome.movedCredentials > 0) {
        summary += tr(" %n credential(s) moved into secrets.", nullptr, outcome.movedCredentials);
    }
    if (outcome.keptCredentials > 0) {
        summary += tr(" %n credential(s) left in plain text.", nullptr, outcome.keptCredentials);
    }
    emit notify(summary, false);
    if (!outcome.notes.isEmpty()) {
        emit importReviewNotes(outcome.notes);
    }
}

void AppController::onLoaded() {
    connect(&activeProject(),
            &ProjectModel::operationRenamed,
            this,
            &AppController::onOperationRenamed,
            Qt::UniqueConnection);
    connect(&activeProject(),
            &ProjectModel::operationRemoved,
            this,
            &AppController::onOperationRemoved,
            Qt::UniqueConnection);
    recordRecentProject(activeProject().name(), activeProject().rootPath());
    persistOpenProjects();
    persistActiveProject();
    rebindActiveProject(/*repopulateTree=*/true);
    selectFirstModule();
    if (const QStringList warnings = loadWarnings(); !warnings.isEmpty()) {
        emit notify(tr("“%1” loaded with %n warning(s); the first: %2", nullptr, warnings.size())
                        .arg(activeProject().name(), warnings.front()),
                    true);
    }
}

void AppController::onSaved() {
    // A mutation keeps the user where they are: refresh models but preserve the selected module and
    // open operation.
    resources_.reload(activeProject().project());
    populateWorkspaceTree();
    status_ = QStringLiteral("%1 modules").arg(resourceCount());
    openExampleStore();

    environments_ = activeProject().environmentNames();
    ensureEnvironmentExists();

    // Keep an open actor detail in sync; clear it if the actor was renamed or deleted. A
    // never-saved draft has nothing saved to follow.
    if (hasActor_ && !selectedActorId_.isEmpty()) {
        const bool stillExists = activeProject().hasProject() &&
                                 activeProject().project().actors.count(
                                     engine::ActorId{selectedActorId_.toStdString()}) > 0;
        if (stillExists) {
            // An edit in progress outlives saves of anything else; saveActorEdits ends it first.
            if (!actorEditing_) {
                prepareEditActor(selectedActorId_);
                selectedActorDescription_ = actorDescription(selectedActorId_);
                selectedActorStrategy_ = actorAuthLabel(selectedActorId_);
                emit actorSelectionChanged();
            }
        } else if (tabs_.valid(activeTabIndex_) &&
                   tabs_.stateAt(activeTabIndex_).kind == TabState::Kind::Actor) {
            closeTabImmediately(activeTabIndex_);
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
    refreshOpenOpExamples();
    emit projectChanged();
    emit selectionChanged();

    // Only when not editing: re-selecting would discard an in-progress edit.
    if (!creatingOperation_ && !editing_ && !openModule.isEmpty() && !openOp.isEmpty()) {
        const auto& resources = activeProject().project().resources;
        const auto it = resources.find(engine::ResourceId{openModule.toStdString()});
        if (it != resources.end() && it->second.operations.contains(openOp.toStdString())) {
            loadOperationReadState(openModule, openOp);
            captureActiveTab();
            emit operationChanged();
            emit chainChanged();
        }
    }
}

void AppController::selectActor(const QString& projectRoot, const QString& actorId) {
    if (!activateForRow(projectRoot,
                        [this, projectRoot, actorId]() { selectActor(projectRoot, actorId); })) {
        return;
    }
    if (!activeProject().hasProject() || actorId.isEmpty()) {
        return;
    }
    openActorTab(projectRoot, actorId, /*isNewDraft=*/false);
}

void AppController::clearActorSelection() {
    const bool deactivatingActorTab{tabs_.valid(activeTabIndex_) &&
                                    tabs_.stateAt(activeTabIndex_).kind == TabState::Kind::Actor};
    if (!deactivatingActorTab && !hasActor_) {
        return;
    }
    if (deactivatingActorTab) {
        captureActiveTab();
        activeTabIndex_ = -1;
    }
    hasActor_ = false;
    selectedActorId_.clear();
    emit actorSelectionChanged();
    if (deactivatingActorTab) {
        persistOpenTabs();
        emit activeTabChanged();
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
    clearActorSelection();
    selectedModule_ = moduleName;
    operations_.reload(it->second);
    closeOperation();
    emit selectionChanged();
}

void AppController::selectOperation(const QString& moduleName, const QString& opName) {
    openOperationTab(activeProject().rootPath(), moduleName, opName);
}

void AppController::closeOperation() {
    const bool deactivatingOperationTab{tabs_.valid(activeTabIndex_) &&
                                        tabs_.stateAt(activeTabIndex_).kind ==
                                            TabState::Kind::Operation};
    const bool parkingOperationDraft{deactivatingOperationTab &&
                                     tabs_.stateAt(activeTabIndex_).operationDraft};
    if (!deactivatingOperationTab && !hasOperation_ && opName_.isEmpty()) {
        return;
    }
    if (deactivatingOperationTab) {
        captureActiveTab();
        activeTabIndex_ = -1;
        if (parkingOperationDraft) {
            clearNewOperationDraftIdentity();
        }
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
    if (deactivatingOperationTab) {
        persistOpenTabs();
        emit activeTabChanged();
    }
}

void AppController::setEnvironment(const QString& env) {
    if (env == environment_) {
        return;
    }
    environment_ = env;
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
    if (!hasOperation_) {
        return;
    }
    if (running_) {
        emit notify(tr("A run is already in progress. Wait for it to finish or cancel it."), true);
        return;
    }
    const QString target = selectedModule_ + '.' + opName_;
    pendingCandidateOp_.clear();
    runController_->run(target, environment_, clean, dryRun);
}

void AppController::setNewOperationName(const QString& name) {
    if (newOperationName_ == name) {
        return;
    }
    newOperationName_ = name;
    if (tabs_.valid(activeTabIndex_)) {
        TabState& tab = tabs_.stateAt(activeTabIndex_);
        if (tab.operationDraft) {
            tab.opName = name;
            const QString trimmed = name.trimmed();
            tab.title = trimmed.isEmpty() ? tr("New endpoint") : trimmed;
            tabs_.refreshRow(activeTabIndex_);
        }
    }
    emit editChanged();
    emit chainChanged();
}

void AppController::setEditMethod(const QString& method) {
    if (method == editMethod_) {
        return;
    }
    editMethod_ = method;
    emit editChanged();
    emit chainChanged();
}

void AppController::setEditPath(const QString& path) {
    if (path == editPath_) {
        return;
    }
    QScopedValueRollback<int> querySync{querySyncDepth_, querySyncDepth_ + 1};
    editPath_ = path;
    editQuery_.setPairs(queryPairsFromVisiblePath(editPath_));
    emit editChanged();
    emit chainChanged();
}

void AppController::setEditActor(const QString& actor) {
    if (actor == editActor_) {
        return;
    }
    editActor_ = actor;
    // Actor and inline auth are mutually exclusive: an actor drops the inline type.
    if (!editActor_.isEmpty()) {
        editAuthType_ = QStringLiteral("none");
    }
    emit editChanged();
    emit chainChanged();
}

void AppController::setEditExpectStatus(const QString& expectStatus) {
    if (expectStatus == editExpectStatus_) {
        return;
    }
    editExpectStatus_ = expectStatus;
    emit editChanged();
    emit chainChanged();
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
    emit chainChanged();
}

void AppController::setEditBodyIsForm(bool isForm) {
    if (isForm == editBodyIsForm_) {
        return;
    }
    editBodyIsForm_ = isForm;
    emit editChanged();
    emit chainChanged();
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
    } else if (type == QStringLiteral("none")) {
        // editBody_ and editForm_ stay in memory so switching back restores them; buildOverride
        // sends neither.
        auto headers = editHeaders_.pairs();
        auto kept = withoutManagedContentType(headers);
        if (kept.size() != headers.size()) {
            editHeaders_.setPairs(std::move(kept));
        }
    }
    emit editChanged();
    emit chainChanged();
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
    // Mutually exclusive with actor: a real inline type clears the actor.
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
    oauthTabUid_ = tabs_.valid(activeTabIndex_) ? tabs_.stateAt(activeTabIndex_).uid : 0;
    oauthFlow_ = std::make_unique<OAuth2AuthCodeFlow>();
    connect(oauthFlow_.get(), &OAuth2AuthCodeFlow::succeeded, this, [this](const QString& token) {
        const int index = tabs_.indexOfUid(oauthTabUid_);
        if (index < 0) {
            emit notify(tr("The OAuth2 token arrived after its tab was closed, so it was dropped."),
                        true);
            return;
        }
        if (index == activeTabIndex_) {
            editAuthOauth2AccessToken_ = token;
            emit editChanged();
        } else {
            tabs_.stateAt(index).editAuthOauth2AccessToken = token;
        }
        emit notify(tr("Access token acquired for “%1”").arg(tabs_.displayTitle(index)), false);
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
    if (creatingOperation_ || !hasOperation_ || !activeProject().hasProject()) {
        return;
    }
    const auto* op =
        activeProject().findOperation(engine::OperationId{currentOperationId().toStdString()});
    if (op == nullptr) {
        return;
    }

    editMethod_ = methodLabel(op->method);
    const auto persistedQuery{toEditPairs(op->queryParams)};
    {
        QScopedValueRollback<int> querySync{querySyncDepth_, querySyncDepth_ + 1};
        editPath_ = visiblePathWithAppendedQueryPairs(QString::fromStdString(op->pathTemplate),
                                                      persistedQuery);
        editQuery_.setPairs(queryPairsFromVisiblePath(editPath_));
    }
    editActor_ = QString::fromStdString(op->actor.value);

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
        // Clamp to known options; an unknown hand-edited value falls back to the default, as the
        // engine does.
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

    // Both set on a hand-edited op: inline auth wins at runtime, so drop the actor from the seed.
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

    // Infer the body kind from Content-Type (raw kinds, multipart vs urlencoded).
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
        const bool anyFile = std::ranges::any_of(
            *op->bodyForm, [](const auto& kv) { return engine::isFormFileTemplate(kv.second); });
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
    prepareChainEditor();
    emit editingChanged();
    emit editChanged();
    emit chainChanged();
}

void AppController::cancelEdit() {
    if (!editing_) {
        return;
    }
    if (creatingOperation_) {
        closeTab(activeTabIndex_);
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
    if (creatingOperation_) {
        emit notify(QStringLiteral("Save the endpoint before changing the project default auth."),
                    true);
        return;
    }
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
    ov.actor = editActor_;
    ov.expectStatus = editExpectStatus_;
    ov.timeoutMs = editTimeout_;
    ov.forceReRun = editForce_;

    // "none" leaves nullopt so applyOverrideToOperation clears prior auth.
    ov.inlineAuth = buildInlineAuthFromEdit();

    applyEditBody(ov, editBodyType_, editBody_, editForm_.pairs());

    // Chain edits are opt-in: a request-only override never wipes depends_on / extract.
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

    // A transient operation has no persisted target, so its Chain tab is the source of truth
    // for depends_on and extract until Save.
    if (creatingOperation_) {
        const QString targetId = currentOperationId();
        for (int i = 0; i < chainEditor_.count(); ++i) {
            if (chainEditor_.operationIdAt(i) != targetId) {
                continue;
            }
            ov.dependencies = chainEditor_.depModelAt(i)->dependencies();
            ov.extractions.clear();
            for (const auto& [variable, sourcePath] : chainEditor_.extractModelAt(i)->pairs()) {
                if (sourcePath.isEmpty()) {
                    continue;
                }
                engine::Extraction extraction;
                extraction.variableName = variable.toStdString();
                extraction.sourcePath = sourcePath.toStdString();
                extraction.source = sourceForPath(extraction.sourcePath);
                ov.extractions.push_back(std::move(extraction));
            }
            break;
        }
    }
    ov.normalizePathQuery();
    return ov;
}

void AppController::applyAndRun(bool clean, bool dryRun) {
    if (creatingOperation_ || !hasOperation_) {
        return;
    }
    if (running_) {
        emit notify(tr("A run is already in progress. Wait for it to finish or cancel it."), true);
        return;
    }
    const QString target = selectedModule_ + '.' + opName_;
    pendingCandidateOp_.clear();
    runController_->runWithOverride(target, environment_, clean, dryRun, buildOverride());
}

void AppController::saveOperation() {
    if (creatingOperation_) {
        saveNewOperation();
        return;
    }
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

    // Patch a copy via the shared override path, then persist; saveOperation validates,
    // so a cycle or undefined op is rejected with disk untouched.
    engine::Operation updated = *op;
    applyOverrideToOperation(updated, buildOverride());

    QString error;
    if (activeProject().saveOperation(id, updated, error)) {
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

    // A file-referenced hook is written back to its `.js`; an inline hook is saved as YAML; empty
    // inline content clears it.
    const QString root = activeProject().rootPath();
    const auto applyHook = [&](std::optional<std::string>& script,
                               const QString& edited,
                               const QString& refPath) -> bool {
        if (!refPath.isEmpty()) {
            // Reject any ref that resolves outside the project root.
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
        // saveOperation resets the selection, so reopen the operation.
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
    refreshActorSessions();
    emit notify(QStringLiteral("Session + extraction caches cleared"), false);
}

QString AppController::currentOperationId() const {
    if (creatingOperation_) {
        return newOperationDraftId_;
    }
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
    const widgets::diff::LineDiffResult diff = widgets::diff::boundedLineDiff(oldText, newText);
    if (diff.tooLarge) {
        return {QVariantMap{
            {QStringLiteral("sign"), QString{}},
            {QStringLiteral("text"),
             tr("These bodies are too large to diff (more than %1 changed lines, or too many "
                "differences).")
                 .arg(widgets::diff::kMaxDiffLines)},
            {QStringLiteral("tooLarge"), true}}};
    }
    QVariantList rows;
    for (const auto& line : diff.lines) {
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

QString AppController::prettyJson(const QString& text, bool unwrapEncoded) const {
    return widgets::body::prettyJson(text, unwrapEncoded);
}

QString AppController::inertPreviewHtml(const QString& source, bool markdown) const {
    return widgets::body::inertRichText(source, markdown);
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

bool AppController::isFormFileValue(const QString& value) const {
    return engine::isFormFileTemplate(value.toStdString());
}

QString AppController::formValueText(const QString& value) const {
    return QString::fromStdString(engine::formTemplateText(value.toStdString()));
}

QString AppController::formTextValue(const QString& text) const {
    return QString::fromStdString(engine::formTextTemplate(text.toStdString()));
}

QVariantList AppController::readBodyFormRows() const {
    QVariantList rows;
    if (!activeProject().hasProject()) {
        return rows;
    }
    const auto& resources = activeProject().project().resources;
    const auto resIt = resources.find(engine::ResourceId{selectedModule_.toStdString()});
    if (resIt == resources.end()) {
        return rows;
    }
    const auto opIt = resIt->second.operations.find(opName_.toStdString());
    if (opIt == resIt->second.operations.end() || !opIt->second.bodyForm) {
        return rows;
    }
    for (const auto& [key, value] : *opIt->second.bodyForm) {
        const bool isFile = engine::isFormFileTemplate(value);
        rows.append(QVariantMap{{QStringLiteral("key"), QString::fromStdString(key)},
                                {QStringLiteral("text"),
                                 isFile ? QString::fromStdString(value.substr(1))
                                        : QString::fromStdString(engine::formTemplateText(value))},
                                {QStringLiteral("isFile"), isFile}});
    }
    return rows;
}

void AppController::refreshExamples() {
    // Rebuilds the tree (resets the model): only on load or example mutation, never a plain switch.
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

void AppController::openExampleStore() {
    const ProjectModel& project = activeProject();
    if (!project.hasProject()) {
        exampleStore_.setProjectRoot({});
        return;
    }
    const QString root = project.rootPath();
    exampleStore_.setProjectRoot(root, QString::fromStdString(project.project().id));
    const SavedResponseStore::LegacyImport legacy = exampleStore_.importLegacyFile();
    const QString file = QDir::toNativeSeparators(legacy.filePath);
    switch (legacy.result) {
        case SavedResponseStore::LegacyImport::Result::Imported:
            emit notify(tr("Saved examples now live outside the project folder (%n imported). %1 "
                           "is no longer used and can be deleted.",
                           nullptr,
                           legacy.imported)
                            .arg(file),
                        false);
            populateWorkspaceTree();
            break;
        case SavedResponseStore::LegacyImport::Result::Unreadable:
            if (!legacyExamplesReported_.contains(root)) {
                legacyExamplesReported_.insert(root);
                emit notify(tr("%1 could not be read (%2), so its examples were not imported. The "
                               "file was left as it is.")
                                .arg(file, legacy.detail),
                            true);
            }
            break;
        case SavedResponseStore::LegacyImport::Result::None:
        case SavedResponseStore::LegacyImport::Result::AlreadyImported:
        case SavedResponseStore::LegacyImport::Result::Failed:
            break;
    }
}

void AppController::refreshActorSessions() {
    QMap<QString, ProjectTreeModel::ActorSessionRow> rows;
    bool anyLive = false;

    if (runController_ && activeProject().hasProject()) {
        const QString root = projectRoot();
        for (const auto& [actorId, unusedActor] : activeProject().project().actors) {
            const RunController::ActorSessionInfo info = runController_->sessionInfo(actorId);
            if (info.state == engine::ActorSession::State::Live) {
                anyLive = true;
            }
            rows.insert(ProjectTreeModel::sessionKey(root, QString::fromStdString(actorId.value)),
                        ProjectTreeModel::ActorSessionRow{sessionStateToken(info.state),
                                                          info.secondsRemaining});
        }
    }
    tree_.setActorSessions(std::move(rows));

    if (anyLive) {
        if (!sessionTicker_.isActive()) {
            sessionTicker_.start();
        }
    } else {
        sessionTicker_.stop();
    }
}

void AppController::onLoadFailed(const QString& code, const QString& detail) {
    projectName_.clear();
    resources_.reset();
    operations_.reset();
    selectedModule_.clear();
    populateWorkspaceTree();
    status_ = QStringLiteral("Load failed (%1): %2").arg(code, detail);
    emit projectChanged();
    emit selectionChanged();
    // status_ is replaced as soon as another open project rebinds, so say it where it is seen.
    emit notify(tr("Could not open the project: %1").arg(detail), true);
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
    // Headers libcurl sends for us; Host is derived at send time.
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
