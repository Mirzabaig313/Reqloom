// AppController — multi-tab editor state (open/activate/close tabs and the
// per-tab snapshot/restore of the single live models + scalars). Split from
// AppController.cpp; see AppController.h.
#include "AppController.h"

#include "application/ProjectModel.h"

#include <reqloom/engine/Operation.h>

#include <QtCore/QLatin1Char>
#include <QtCore/QSettings>
#include <QtCore/QString>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "AppControllerInternal.h"

namespace reqloom::desktop::qml {

namespace {

/// Convert a QString dependency list to the engine's std::string form.
[[nodiscard]] std::vector<std::string> toStdDeps(const std::vector<QString>& deps) {
    std::vector<std::string> out;
    out.reserve(deps.size());
    for (const auto& d : deps) {
        out.push_back(d.toStdString());
    }
    return out;
}

/// Convert the engine's std::string dependency list to QString form.
[[nodiscard]] std::vector<QString> toQDeps(const std::vector<std::string>& deps) {
    std::vector<QString> out;
    out.reserve(deps.size());
    for (const auto& d : deps) {
        out.push_back(QString::fromStdString(d));
    }
    return out;
}

// Open tabs persist under one settings group, keyed by the project root. As in
// EnvironmentSettings, '/' in the key is remapped to '|' so QSettings doesn't
// treat a POSIX root as a nested path.
constexpr const char* kTabsGroup = "openTabs";

[[nodiscard]] QString sanitizeRoot(const QString& root) {
    QString key = root;
    key.replace(QLatin1Char('/'), QLatin1Char('|'));
    return key;
}

}  // namespace

bool AppController::loadOperationReadState(const QString& moduleName, const QString& opName) {
    if (!activeProject().hasProject()) {
        return false;
    }
    const auto& resources = activeProject().project().resources;
    const auto resIt = resources.find(engine::ResourceId{moduleName.toStdString()});
    if (resIt == resources.end()) {
        return false;
    }
    const auto opIt = resIt->second.operations.find(opName.toStdString());
    if (opIt == resIt->second.operations.end()) {
        return false;
    }
    const engine::Operation op = opIt->second;

    selectedModule_ = moduleName;
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
    return true;
}

void AppController::captureOperationInto(TabState& tab) const {
    tab.editing = editing_;
    tab.editMethod = editMethod_;
    tab.editPath = editPath_;
    tab.editActor = editActor_;
    tab.editExpectStatus = editExpectStatus_;
    tab.editTimeout = editTimeout_;
    tab.editForce = editForce_;
    tab.editBody = editBody_;
    tab.editBodyIsForm = editBodyIsForm_;
    tab.editBodyType = editBodyType_;
    tab.chainFieldsLoaded = chainFieldsLoaded_;

    tab.editAuthType = editAuthType_;
    tab.editAuthToken = editAuthToken_;
    tab.editAuthUsername = editAuthUsername_;
    tab.editAuthPassword = editAuthPassword_;
    tab.editAuthApiKeyName = editAuthApiKeyName_;
    tab.editAuthApiKeyValue = editAuthApiKeyValue_;
    tab.editAuthApiKeyInQuery = editAuthApiKeyInQuery_;
    tab.editAuthAwsAccessKey = editAuthAwsAccessKey_;
    tab.editAuthAwsSecretKey = editAuthAwsSecretKey_;
    tab.editAuthAwsRegion = editAuthAwsRegion_;
    tab.editAuthAwsService = editAuthAwsService_;
    tab.editAuthAwsSessionToken = editAuthAwsSessionToken_;
    tab.editAuthOauthConsumerKey = editAuthOauthConsumerKey_;
    tab.editAuthOauthConsumerSecret = editAuthOauthConsumerSecret_;
    tab.editAuthOauthToken = editAuthOauthToken_;
    tab.editAuthOauthTokenSecret = editAuthOauthTokenSecret_;
    tab.editAuthOauth2GrantType = editAuthOauth2GrantType_;
    tab.editAuthOauth2ClientAuth = editAuthOauth2ClientAuth_;
    tab.editAuthOauth2AuthUrl = editAuthOauth2AuthUrl_;
    tab.editAuthOauth2CallbackUrl = editAuthOauth2CallbackUrl_;
    tab.editAuthOauth2PkceMethod = editAuthOauth2PkceMethod_;
    tab.editAuthOauth2AccessToken = editAuthOauth2AccessToken_;
    tab.editAuthOauth2TokenUrl = editAuthOauth2TokenUrl_;
    tab.editAuthOauth2ClientId = editAuthOauth2ClientId_;
    tab.editAuthOauth2ClientSecret = editAuthOauth2ClientSecret_;
    tab.editAuthOauth2Scope = editAuthOauth2Scope_;
    tab.editAuthJwtAlgorithm = editAuthJwtAlgorithm_;
    tab.editAuthJwtSecret = editAuthJwtSecret_;
    tab.editAuthJwtPayload = editAuthJwtPayload_;
    tab.editAuthMtlsFormat = editAuthMtlsFormat_;
    tab.editAuthMtlsCertPath = editAuthMtlsCertPath_;
    tab.editAuthMtlsKeyPath = editAuthMtlsKeyPath_;
    tab.editAuthMtlsKeyPassword = editAuthMtlsKeyPassword_;
    tab.editAuthMtlsCaCertPath = editAuthMtlsCaCertPath_;

    tab.editHeaders = editHeaders_.pairs();
    tab.editQuery = editQuery_.pairs();
    tab.editForm = editForm_.pairs();
    tab.editExtractions = editExtractions_.pairs();
    tab.editAssertions = editAssertions_.pairs();
    tab.editDependencies = toQDeps(editDependencies_.dependencies());

    tab.hasResponse = hasResponse_;
    tab.respStatus = respStatus_;
    tab.respElapsedMs = respElapsedMs_;
    tab.respBodySize = respBodySize_;
    tab.respHeaders = respHeaders_;
    tab.respBody = respBody_;
    tab.runOutcome = runOutcome_;
    tab.shownExample = shownExample_;
}

void AppController::restoreOperationEditFrom(const TabState& tab) {
    editing_ = tab.editing;
    editMethod_ = tab.editMethod;
    editPath_ = tab.editPath;
    editActor_ = tab.editActor;
    editExpectStatus_ = tab.editExpectStatus;
    editTimeout_ = tab.editTimeout;
    editForce_ = tab.editForce;
    editBody_ = tab.editBody;
    editBodyIsForm_ = tab.editBodyIsForm;
    editBodyType_ = tab.editBodyType;
    chainFieldsLoaded_ = tab.chainFieldsLoaded;

    editAuthType_ = tab.editAuthType;
    editAuthToken_ = tab.editAuthToken;
    editAuthUsername_ = tab.editAuthUsername;
    editAuthPassword_ = tab.editAuthPassword;
    editAuthApiKeyName_ = tab.editAuthApiKeyName;
    editAuthApiKeyValue_ = tab.editAuthApiKeyValue;
    editAuthApiKeyInQuery_ = tab.editAuthApiKeyInQuery;
    editAuthAwsAccessKey_ = tab.editAuthAwsAccessKey;
    editAuthAwsSecretKey_ = tab.editAuthAwsSecretKey;
    editAuthAwsRegion_ = tab.editAuthAwsRegion;
    editAuthAwsService_ = tab.editAuthAwsService;
    editAuthAwsSessionToken_ = tab.editAuthAwsSessionToken;
    editAuthOauthConsumerKey_ = tab.editAuthOauthConsumerKey;
    editAuthOauthConsumerSecret_ = tab.editAuthOauthConsumerSecret;
    editAuthOauthToken_ = tab.editAuthOauthToken;
    editAuthOauthTokenSecret_ = tab.editAuthOauthTokenSecret;
    editAuthOauth2GrantType_ = tab.editAuthOauth2GrantType;
    editAuthOauth2ClientAuth_ = tab.editAuthOauth2ClientAuth;
    editAuthOauth2AuthUrl_ = tab.editAuthOauth2AuthUrl;
    editAuthOauth2CallbackUrl_ = tab.editAuthOauth2CallbackUrl;
    editAuthOauth2PkceMethod_ = tab.editAuthOauth2PkceMethod;
    editAuthOauth2AccessToken_ = tab.editAuthOauth2AccessToken;
    editAuthOauth2TokenUrl_ = tab.editAuthOauth2TokenUrl;
    editAuthOauth2ClientId_ = tab.editAuthOauth2ClientId;
    editAuthOauth2ClientSecret_ = tab.editAuthOauth2ClientSecret;
    editAuthOauth2Scope_ = tab.editAuthOauth2Scope;
    editAuthJwtAlgorithm_ = tab.editAuthJwtAlgorithm;
    editAuthJwtSecret_ = tab.editAuthJwtSecret;
    editAuthJwtPayload_ = tab.editAuthJwtPayload;
    editAuthMtlsFormat_ = tab.editAuthMtlsFormat;
    editAuthMtlsCertPath_ = tab.editAuthMtlsCertPath;
    editAuthMtlsKeyPath_ = tab.editAuthMtlsKeyPath;
    editAuthMtlsKeyPassword_ = tab.editAuthMtlsKeyPassword;
    editAuthMtlsCaCertPath_ = tab.editAuthMtlsCaCertPath;

    editHeaders_.setPairs(tab.editHeaders);
    editQuery_.setPairs(tab.editQuery);
    editForm_.setPairs(tab.editForm);
    editExtractions_.setPairs(tab.editExtractions);
    editAssertions_.setPairs(tab.editAssertions);
    editDependencies_.setCandidates(editDependencyCandidates());
    editDependencies_.setDependencies(toStdDeps(tab.editDependencies));

    hasResponse_ = tab.hasResponse;
    respStatus_ = tab.respStatus;
    respElapsedMs_ = tab.respElapsedMs;
    respBodySize_ = tab.respBodySize;
    respHeaders_ = tab.respHeaders;
    respBody_ = tab.respBody;
    responseBody_.setBody(tab.respBody);
    runOutcome_ = tab.runOutcome;
    shownExample_ = tab.shownExample;
}

void AppController::captureActorInto(TabState& tab) const {
    tab.actorId = selectedActorId_;
    tab.actorName = selectedActorName_;
    tab.actorDescription = selectedActorDescription_;
    tab.actorStrategy = selectedActorStrategy_;
    tab.actorConfig = actorConfig_.pairs();
    tab.actorAuthExtract = actorAuthExtract_.pairs();
    tab.actorRefreshExtract = actorRefreshExtract_.pairs();

    tab.actorAuthSteps.clear();
    for (int i = 0; i < actorAuthSteps_.count(); ++i) {
        AuthStepListModel::StepSeed seed;
        seed.id = actorAuthSteps_.idAt(i);
        seed.method = actorAuthSteps_.methodAt(i);
        seed.path = actorAuthSteps_.pathAt(i);
        seed.body = actorAuthSteps_.bodyAt(i);
        seed.expect = actorAuthSteps_.expectAt(i);
        if (const auto* extract = actorAuthSteps_.extractModelAt(i); extract != nullptr) {
            seed.extractions = extract->pairs();
        }
        tab.actorAuthSteps.push_back(std::move(seed));
    }

    tab.actorAuthMethod = actorAuthMethod_;
    tab.actorAuthPath = actorAuthPath_;
    tab.actorAuthBody = actorAuthBody_;
    tab.actorAuthExpect = actorAuthExpect_;
    tab.actorHasRefresh = actorHasRefresh_;
    tab.actorRefreshMethod = actorRefreshMethod_;
    tab.actorRefreshPath = actorRefreshPath_;
    tab.actorRefreshBody = actorRefreshBody_;
}

void AppController::restoreActorFrom(const TabState& tab) {
    selectedActorId_ = tab.actorId;
    selectedActorName_ = tab.actorName;
    selectedActorDescription_ = tab.actorDescription;
    selectedActorStrategy_ = tab.actorStrategy;
    actorConfig_.setPairs(tab.actorConfig);
    actorAuthExtract_.setPairs(tab.actorAuthExtract);
    actorRefreshExtract_.setPairs(tab.actorRefreshExtract);
    actorAuthSteps_.rebuild(tab.actorAuthSteps);
    actorAuthMethod_ = tab.actorAuthMethod;
    actorAuthPath_ = tab.actorAuthPath;
    actorAuthBody_ = tab.actorAuthBody;
    actorAuthExpect_ = tab.actorAuthExpect;
    actorHasRefresh_ = tab.actorHasRefresh;
    actorRefreshMethod_ = tab.actorRefreshMethod;
    actorRefreshPath_ = tab.actorRefreshPath;
    actorRefreshBody_ = tab.actorRefreshBody;
}

void AppController::captureActiveTab() {
    if (!tabs_.valid(activeTabIndex_)) {
        return;
    }
    TabState& tab = tabs_.stateAt(activeTabIndex_);
    if (tab.kind == TabState::Kind::Operation) {
        captureOperationInto(tab);
        if (tab.operationDraft) {
            tab.id = newOperationDraftId_;
            tab.module = newOperationModule_;
            tab.opName = newOperationName_;
            const QString trimmedName = newOperationName_.trimmed();
            tab.title = trimmedName.isEmpty() ? tr("New endpoint") : trimmedName;
            tab.method = editMethod_;
            tab.subtitle = editPath_;
            tab.dirty = true;
        } else {
            // The read fields can shift after a save; keep the strip label current.
            tab.method = opMethod_;
            tab.subtitle = opPath_;
            tab.dirty = editing_;  // v1: "in edit mode" is the unsaved proxy
        }
        // Snapshot the whole-chain editor so unsaved per-step wiring survives a
        // switch. Only meaningful once the chain has been seeded (edit mode).
        if (chainFieldsLoaded_ && chainEditor_.count() > 0) {
            tab.chainSeeds = chainEditor_.snapshotSeeds();
            tab.chainSnapshotValid = true;
        } else {
            tab.chainSnapshotValid = false;
        }
    } else {
        captureActorInto(tab);
        tab.title = selectedActorName_.isEmpty() ? tr("New actor") : selectedActorName_;
        tab.subtitle = selectedActorStrategy_;
        tab.dirty = selectedActorId_.isEmpty();  // never-saved draft
    }
    tab.timeline = timeline_.takeSnapshot();
    tabs_.refreshRow(activeTabIndex_);
}

void AppController::updateActiveTabDirty() {
    if (!tabs_.valid(activeTabIndex_)) {
        return;
    }
    TabState& tab = tabs_.stateAt(activeTabIndex_);
    const bool dirty = (tab.kind == TabState::Kind::Operation) ? (tab.operationDraft || editing_)
                                                               : tab.id.isEmpty();
    if (tab.dirty != dirty) {
        tab.dirty = dirty;
        tabs_.refreshRow(activeTabIndex_);
    }
}

void AppController::restoreActiveTab() {
    if (!tabs_.valid(activeTabIndex_)) {
        syncActiveKindFlags();
        return;
    }
    const TabState tab = tabs_.stateAt(activeTabIndex_);
    // Restore this tab's own run timeline (parked on the last switch away).
    // Response body/status are also per tab.
    timeline_.restoreSnapshot(tab.timeline);
    if (tab.kind == TabState::Kind::Operation) {
        hasActor_ = false;
        if (tab.operationDraft) {
            creatingOperation_ = true;
            newOperationModule_ = tab.module;
            newOperationDraftId_ = tab.id;
            newOperationName_ = tab.opName;
            selectedModule_ = tab.module;
            opName_ = tab.opName;
            opMethod_ = tab.editMethod;
            opPath_ = tab.editPath;
            opActor_ = tab.editActor;
            opBody_ = tab.editBody;
            opDependencies_.clear();
            opHeaders_.reset();
            opQuery_.reset();
            opExtractions_.reset();
            opAssertions_.reset();
            exampleList_.clear();
            hasOperation_ = true;
        } else {
            clearNewOperationDraftIdentity();
            // Read fields come from the project (they only change on save); the
            // edit/response snapshot is overlaid on top.
            loadOperationReadState(tab.module, tab.opName);
        }
        restoreOperationEditFrom(tab);
        if (editing_) {
            if (tab.chainSnapshotValid) {
                // Restore the tab's own unsaved chain wiring verbatim.
                chainEditor_.rebuild(tab.chainSeeds);
            } else if (!tab.operationDraft) {
                // No snapshot (first time in edit mode): derive from saved deps.
                prepareChainEditor();
            }
        }
    } else {
        clearNewOperationDraftIdentity();
        hasOperation_ = false;
        editing_ = false;
        chainFieldsLoaded_ = false;
        chainEditor_.rebuild({});
        restoreActorFrom(tab);
    }
    emit operationChanged();
    emit editingChanged();
    emit editChanged();
    emit actorSelectionChanged();
    emit actorEditChanged();
    emit responseChanged();
    emit chainChanged();
    emit selectionChanged();
    refreshOpenOpExamples();
}

void AppController::syncActiveKindFlags() {
    if (tabs_.valid(activeTabIndex_)) {
        const bool isOp = tabs_.stateAt(activeTabIndex_).kind == TabState::Kind::Operation;
        hasOperation_ = isOp;
        hasActor_ = !isOp;
    } else {
        hasOperation_ = false;
        hasActor_ = false;
        opName_.clear();
        selectedActorId_.clear();
    }
    emit operationChanged();
    emit actorSelectionChanged();
}

void AppController::openOperationTab(const QString& projectRoot,
                                     const QString& moduleName,
                                     const QString& opName) {
    const QString opId = moduleName + QLatin1Char('.') + opName;
    if (const int existing = tabs_.indexOf(TabState::Kind::Operation, opId); existing >= 0) {
        activateTab(existing);
        return;
    }

    captureActiveTab();

    if (!loadOperationReadState(moduleName, opName)) {
        return;  // operation no longer exists
    }
    clearNewOperationDraftIdentity();
    hasActor_ = false;
    editing_ = false;
    chainFieldsLoaded_ = false;
    // A freshly opened tab starts with an empty response of its own.
    hasResponse_ = false;
    respStatus_ = 0;
    respElapsedMs_ = 0;
    respBodySize_ = 0;
    respHeaders_.clear();
    respBody_.clear();
    responseBody_.setBody(QString{});
    runOutcome_.clear();
    shownExample_.clear();
    // A new tab has no run of its own yet; start its timeline empty so the
    // snapshot below doesn't inherit the previously active tab's timeline.
    timeline_.reset();

    TabState tab;
    tab.kind = TabState::Kind::Operation;
    tab.id = opId;
    tab.module = moduleName;
    tab.opName = opName;
    tab.projectRoot = projectRoot;
    tab.title = opName;
    tab.method = opMethod_;
    tab.subtitle = opPath_;
    activeTabIndex_ = tabs_.append(std::move(tab));
    captureActiveTab();  // snapshot the freshly loaded live state into the tab

    emit operationChanged();
    emit editingChanged();
    emit editChanged();
    emit actorSelectionChanged();
    emit responseChanged();
    emit chainChanged();
    emit selectionChanged();
    emit activeTabChanged();
    refreshOpenOpExamples();
    persistOpenTabs();
}

void AppController::openActorTab(const QString& projectRoot,
                                 const QString& actorId,
                                 bool isNewDraft) {
    if (!isNewDraft) {
        if (const int existing = tabs_.indexOf(TabState::Kind::Actor, actorId); existing >= 0) {
            activateTab(existing);
            return;
        }
    }

    captureActiveTab();
    clearNewOperationDraftIdentity();
    editing_ = false;
    chainFieldsLoaded_ = false;
    chainEditor_.rebuild({});

    if (isNewDraft) {
        prepareNewActor();
        selectedActorId_.clear();
        selectedActorName_.clear();
        selectedActorDescription_.clear();
        selectedActorStrategy_ = QStringLiteral("Multi-step login chain");
    } else {
        prepareEditActor(actorId);
        selectedActorId_ = actorId;
        selectedActorName_ = actorId;
        selectedActorDescription_ = actorDescription(actorId);
        selectedActorStrategy_ = actorAuthLabel(actorId);
    }
    hasActor_ = true;
    hasOperation_ = false;
    // A new actor tab has no run timeline of its own yet.
    timeline_.reset();

    TabState tab;
    tab.kind = TabState::Kind::Actor;
    tab.id = actorId;  // empty for a new draft
    tab.projectRoot = projectRoot;
    tab.title = isNewDraft ? tr("New actor") : actorId;
    tab.subtitle = selectedActorStrategy_;
    activeTabIndex_ = tabs_.append(std::move(tab));
    captureActiveTab();

    emit operationChanged();
    emit editingChanged();
    emit editChanged();
    emit actorSelectionChanged();
    emit actorEditChanged();
    emit chainChanged();
    emit activeTabChanged();
    persistOpenTabs();
}

void AppController::moveTab(int from, int to) {
    if (from == to || !tabs_.valid(from) || !tabs_.valid(to)) {
        return;
    }
    // Track the active tab across the reorder so it stays selected. Only the
    // ordering changes — the live editor state doesn't, so no restore needed.
    const int active = activeTabIndex_;
    tabs_.move(from, to);
    if (active == from) {
        activeTabIndex_ = to;
    } else if (from < active && active <= to) {
        activeTabIndex_ = active - 1;
    } else if (to <= active && active < from) {
        activeTabIndex_ = active + 1;
    }
    persistOpenTabs();
    emit activeTabChanged();
}

void AppController::activateTab(int index) {
    if (index == activeTabIndex_ || !tabs_.valid(index)) {
        return;
    }
    captureActiveTab();
    activeTabIndex_ = index;
    restoreActiveTab();
    persistOpenTabs();
    emit activeTabChanged();
}

void AppController::closeTab(int index) {
    if (!tabs_.valid(index)) {
        return;
    }
    if (tabs_.stateAt(index).operationDraft) {
        pendingDraftClose_ = PendingDraftClose::SingleTab;
        pendingDraftCloseKeepTab_.reset();
        emit newOperationDiscardRequested();
        return;
    }
    closeTabImmediately(index);
}

void AppController::requestDiscardNewOperation() {
    if (const int draftIndex = operationDraftIndex(); draftIndex >= 0) {
        closeTab(draftIndex);
    }
}

void AppController::closeTabImmediately(int index) {
    if (!tabs_.valid(index)) {
        return;
    }
    const bool closingActive = index == activeTabIndex_;
    const bool closingDraft = tabs_.stateAt(index).operationDraft;
    tabs_.removeAt(index);

    if (tabs_.count() == 0) {
        activeTabIndex_ = -1;
        if (closingDraft) {
            clearNewOperationDraftIdentity();
            restoreOperationEditFrom(TabState{});
            chainEditor_.rebuild({});
            timeline_.reset();
            const auto& resources = activeProject().project().resources;
            if (const auto resource =
                    resources.find(engine::ResourceId{selectedModule_.toStdString()});
                resource != resources.end()) {
                operations_.reload(resource->second);
            } else {
                operations_.reset();
            }
            emit editingChanged();
            emit editChanged();
            emit responseChanged();
            emit chainChanged();
            emit selectionChanged();
        }
        syncActiveKindFlags();
        persistOpenTabs();
        emit activeTabChanged();
        return;
    }

    if (closingActive) {
        activeTabIndex_ = std::min(index, tabs_.count() - 1);
        restoreActiveTab();
    } else if (index < activeTabIndex_) {
        // A tab before the active one shifted every later index down by one.
        --activeTabIndex_;
    }
    persistOpenTabs();
    emit activeTabChanged();
}

void AppController::closeOtherTabs(int index) {
    if (!tabs_.valid(index) || tabs_.count() <= 1) {
        return;
    }
    const int draftIndex = operationDraftIndex();
    if (draftIndex >= 0 && draftIndex != index) {
        if (index == activeTabIndex_) {
            captureActiveTab();
        }
        pendingDraftClose_ = PendingDraftClose::CloseOtherTabs;
        pendingDraftCloseKeepTab_ = tabs_.stateAt(index);
        emit newOperationDiscardRequested();
        return;
    }
    closeOtherTabsImmediately(index);
}

void AppController::closeOtherTabsImmediately(int index) {
    if (!tabs_.valid(index) || tabs_.count() <= 1) {
        return;
    }
    activateTab(index);  // make it active + live shows it
    captureActiveTab();  // ensure its buffer is current
    TabState keep = tabs_.stateAt(activeTabIndex_);
    tabs_.clearAll();
    activeTabIndex_ = tabs_.append(std::move(keep));
    persistOpenTabs();
    emit activeTabChanged();
}

void AppController::confirmDiscardNewOperation() {
    const PendingDraftClose action = pendingDraftClose_;
    std::optional<TabState> keepTab = std::move(pendingDraftCloseKeepTab_);
    keepEditingNewOperation();

    if (action == PendingDraftClose::SingleTab) {
        if (const int draftIndex = operationDraftIndex(); draftIndex >= 0) {
            closeTabImmediately(draftIndex);
            emit newOperationDraftDiscarded();
        }
        return;
    }
    if (action == PendingDraftClose::CloseOtherTabs && keepTab && operationDraftIndex() >= 0) {
        tabs_.clearAll();
        activeTabIndex_ = tabs_.append(std::move(*keepTab));
        restoreActiveTab();
        persistOpenTabs();
        emit activeTabChanged();
        emit newOperationDraftDiscarded();
    }
}

void AppController::keepEditingNewOperation() {
    pendingDraftClose_ = PendingDraftClose::None;
    pendingDraftCloseKeepTab_.reset();
}

int AppController::operationDraftIndex() const {
    for (int i = 0; i < tabs_.count(); ++i) {
        if (tabs_.stateAt(i).operationDraft) {
            return i;
        }
    }
    return -1;
}

void AppController::persistOpenTabs() const {
    if (restoringTabs_) {
        return;  // a restore is re-opening tabs; it persists once at the end
    }
    const QString root = activeProject().rootPath();
    if (root.isEmpty()) {
        return;  // no project → nothing to key the tabs against
    }
    const QString key = sanitizeRoot(root);

    // Collect the reopenable tabs (skip never-saved actor drafts — they have no
    // id to reload) and note which persisted slot is the active one.
    struct Entry {
        int kind{0};
        QString id;
    };
    std::vector<Entry> entries;
    int activeKind = -1;
    QString activeId;
    const auto isReopenable = [](const TabState& tab) {
        return !tab.operationDraft && !(tab.kind == TabState::Kind::Actor && tab.id.isEmpty());
    };
    const auto rememberActive = [&activeKind, &activeId](const TabState& tab) {
        activeKind = static_cast<int>(tab.kind);
        activeId = tab.id;
    };
    for (int i = 0; i < tabs_.count(); ++i) {
        const TabState& tab = tabs_.stateAt(i);
        if (!isReopenable(tab)) {
            continue;
        }
        if (i == activeTabIndex_) {
            rememberActive(tab);
        }
        entries.push_back(Entry{static_cast<int>(tab.kind), tab.id});
    }
    // Transient drafts are intentionally omitted. Persist the nearest tab that
    // can actually reopen so restart never records an unusable active identity.
    if (activeKind < 0 && tabs_.valid(activeTabIndex_)) {
        for (int i = activeTabIndex_ - 1; i >= 0; --i) {
            const TabState& tab = tabs_.stateAt(i);
            if (isReopenable(tab)) {
                rememberActive(tab);
                break;
            }
        }
        for (int i = activeTabIndex_ + 1; activeKind < 0 && i < tabs_.count(); ++i) {
            const TabState& tab = tabs_.stateAt(i);
            if (isReopenable(tab)) {
                rememberActive(tab);
                break;
            }
        }
    }

    QSettings settings;
    settings.beginGroup(QString::fromUtf8(kTabsGroup));
    settings.remove(key);
    settings.remove(key + QStringLiteral("|activeKind"));
    settings.remove(key + QStringLiteral("|activeId"));
    settings.beginWriteArray(key);
    for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
        settings.setArrayIndex(i);
        settings.setValue(QStringLiteral("kind"), entries[static_cast<std::size_t>(i)].kind);
        settings.setValue(QStringLiteral("id"), entries[static_cast<std::size_t>(i)].id);
    }
    settings.endArray();
    // Persist the active tab's identity (kind + id), not its position, so a tab
    // deleted between sessions can't shift which tab reactivates.
    settings.setValue(key + QStringLiteral("|activeKind"), activeKind);
    settings.setValue(key + QStringLiteral("|activeId"), activeId);
    settings.endGroup();
    // No explicit sync() here: tab switches are frequent, and losing the very
    // latest strip state on a hard crash is harmless. Qt flushes on exit.
}

void AppController::restoreOpenTabs(const QString& projectRoot) {
    if (projectRoot.isEmpty()) {
        return;
    }
    const QString key = sanitizeRoot(projectRoot);

    struct Entry {
        int kind{0};
        QString id;
    };
    std::vector<Entry> entries;
    int activeKind = -1;
    QString activeId;
    {
        QSettings settings;
        settings.beginGroup(QString::fromUtf8(kTabsGroup));
        const int count = settings.beginReadArray(key);
        entries.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            settings.setArrayIndex(i);
            Entry entry;
            entry.kind = settings.value(QStringLiteral("kind")).toInt();
            entry.id = settings.value(QStringLiteral("id")).toString();
            if (!entry.id.isEmpty()) {
                entries.push_back(std::move(entry));
            }
        }
        settings.endArray();
        activeKind = settings.value(key + QStringLiteral("|activeKind"), -1).toInt();
        activeId = settings.value(key + QStringLiteral("|activeId")).toString();
        settings.endGroup();
    }

    if (entries.empty()) {
        return;
    }

    // Re-open each saved tab. openOperationTab drops a since-deleted op (its
    // read-state load fails), so stale tabs fall away like stale recents.
    restoringTabs_ = true;
    for (const Entry& entry : entries) {
        if (entry.kind == static_cast<int>(TabState::Kind::Operation)) {
            const qsizetype dot = entry.id.indexOf(QLatin1Char('.'));
            if (dot <= 0) {
                continue;  // malformed id
            }
            openOperationTab(projectRoot, entry.id.left(dot), entry.id.mid(dot + 1));
        } else {
            // Skip a since-deleted actor so it doesn't reopen as a blank tab.
            if (activeProject().hasProject() && activeProject().project().actors.count(
                                                    engine::ActorId{entry.id.toStdString()}) == 0) {
                continue;
            }
            openActorTab(projectRoot, entry.id, /*isNewDraft=*/false);
        }
    }
    restoringTabs_ = false;
    // Activate the saved tab by its identity (kind + id), so a tab deleted
    // between sessions can't shift which one reactivates. Fall back to the last
    // opened tab when the saved active tab is gone.
    if (tabs_.count() > 0) {
        int target = -1;
        if (activeKind >= 0 && !activeId.isEmpty()) {
            target = tabs_.indexOf(static_cast<TabState::Kind>(activeKind), activeId);
        }
        activeTabIndex_ = tabs_.valid(target) ? target : tabs_.count() - 1;
        restoreActiveTab();
    }
    persistOpenTabs();  // reconcile settings with what actually re-opened
    emit activeTabChanged();
}

}  // namespace reqloom::desktop::qml
