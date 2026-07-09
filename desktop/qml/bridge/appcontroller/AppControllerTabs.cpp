// AppController — multi-tab editor state (open/activate/close tabs and the
// per-tab snapshot/restore of the single live models + scalars). Split from
// AppController.cpp; see AppController.h.
#include "AppController.h"

#include "application/ProjectModel.h"

#include <reqloom/engine/Operation.h>

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
    const engine::Operation& op = opIt->second;

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
        // The read fields can shift after a save; keep the strip label current.
        tab.method = opMethod_;
        tab.subtitle = opPath_;
        tab.dirty = editing_;  // v1: "in edit mode" is the unsaved proxy
    } else {
        captureActorInto(tab);
        tab.title = selectedActorName_.isEmpty() ? tr("New actor") : selectedActorName_;
        tab.subtitle = selectedActorStrategy_;
        tab.dirty = selectedActorId_.isEmpty();  // never-saved draft
    }
    tabs_.refreshRow(activeTabIndex_);
}

void AppController::updateActiveTabDirty() {
    if (!tabs_.valid(activeTabIndex_)) {
        return;
    }
    TabState& tab = tabs_.stateAt(activeTabIndex_);
    const bool dirty = (tab.kind == TabState::Kind::Operation) ? editing_
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
    const TabState& tab = tabs_.stateAt(activeTabIndex_);
    // The live run timeline isn't snapshotted per tab (v1); reset it on switch
    // so the Timeline tab shows a clean slate for the newly-active tab rather
    // than a different endpoint's run. Response body/status ARE per tab.
    // ponytail: upgrade path is to snapshot TimelineModel's event rows per tab.
    timeline_.reset();
    if (tab.kind == TabState::Kind::Operation) {
        hasActor_ = false;
        // Read fields come from the project (they only change on save); the
        // edit/response snapshot is overlaid on top.
        loadOperationReadState(tab.module, tab.opName);
        restoreOperationEditFrom(tab);
        if (editing_) {
            // The Chain tab is re-derived from the (restored) deps rather than
            // snapshotted. ponytail: unsaved per-step chain-extract edits are
            // re-derived from saved deps on tab switch; upgrade path is to
            // snapshot ChainEditorModel's nested rows.
            prepareChainEditor();
        }
    } else {
        hasOperation_ = false;
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

    TabState tab;
    tab.kind = TabState::Kind::Actor;
    tab.id = actorId;  // empty for a new draft
    tab.projectRoot = projectRoot;
    tab.title = isNewDraft ? tr("New actor") : actorId;
    tab.subtitle = selectedActorStrategy_;
    activeTabIndex_ = tabs_.append(std::move(tab));
    captureActiveTab();

    emit operationChanged();
    emit actorSelectionChanged();
    emit actorEditChanged();
    emit activeTabChanged();
}

void AppController::activateTab(int index) {
    if (index == activeTabIndex_ || !tabs_.valid(index)) {
        return;
    }
    captureActiveTab();
    activeTabIndex_ = index;
    restoreActiveTab();
    emit activeTabChanged();
}

void AppController::closeTab(int index) {
    if (!tabs_.valid(index)) {
        return;
    }
    const bool closingActive = (index == activeTabIndex_);
    tabs_.removeAt(index);

    if (tabs_.count() == 0) {
        activeTabIndex_ = -1;
        syncActiveKindFlags();
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
    emit activeTabChanged();
}

void AppController::closeOtherTabs(int index) {
    if (!tabs_.valid(index) || tabs_.count() <= 1) {
        return;
    }
    activateTab(index);  // make it active + live shows it
    captureActiveTab();  // ensure its buffer is current
    TabState keep = tabs_.stateAt(activeTabIndex_);
    tabs_.clearAll();
    activeTabIndex_ = tabs_.append(std::move(keep));
    emit activeTabChanged();
}

}  // namespace reqloom::desktop::qml
