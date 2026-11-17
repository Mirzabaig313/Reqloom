// AppController — the QML-facing application controller. Owns the ProjectModel logic and the
// ResourceListModel; QML binds to its properties.
#pragma once

#include "AuthStepListModel.h"
#include "ChainEditorModel.h"
#include "DependencyEditModel.h"
#include "EditableKeyValueModel.h"
#include "EnvironmentEdits.h"
#include "ExampleListModel.h"
#include "HistoryModel.h"
#include "KeyValueModel.h"
#include "OperationListModel.h"
#include "ProjectTreeFilterModel.h"
#include "ProjectTreeModel.h"
#include "ResourceListModel.h"
#include "ResponseBodyModel.h"
#include "RunTabRouter.h"
#include "TabEdits.h"
#include "TabModel.h"
#include "TimelineModel.h"
#include "application/Bootstrapper.h"
#include "application/EnvironmentSettings.h"
#include "application/ImportJob.h"
#include "application/ProjectEdits.h"
#include "application/RunController.h"
#include "application/SavedResponseStore.h"

#include <reqloom/engine/HttpDefaults.h>

#include <QtQml/qqmlregistration.h>
#include <QtCore/QAbstractItemModel>
#include <QtCore/QFutureWatcher>
#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QSet>
#include <QtCore/QSettings>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QVariant>

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <cstdint>

namespace reqloom::desktop {
class ProjectModel;
class WorkspaceModel;
class OAuth2AuthCodeFlow;
}  // namespace reqloom::desktop

class QQmlEngine;
class QJSEngine;

namespace reqloom::desktop::qml {

class AppController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QString projectName READ projectName NOTIFY projectChanged)
    Q_PROPERTY(QString projectRoot READ projectRoot NOTIFY projectChanged)
    Q_PROPERTY(int resourceCount READ resourceCount NOTIFY projectChanged)
    Q_PROPERTY(int latencySloP95Ms READ latencySloP95Ms NOTIFY projectChanged)
    Q_PROPERTY(QString status READ status NOTIFY projectChanged)
    Q_PROPERTY(ResourceListModel* resources READ resources CONSTANT)
    Q_PROPERTY(OperationListModel* operations READ operations CONSTANT)
    Q_PROPERTY(QString selectedModule READ selectedModule NOTIFY selectionChanged)

    // ── Open editor tabs (endpoints + actors) ──
    // Each tab keeps a snapshot of editor state; switching swaps the single live models/scalars.
    Q_PROPERTY(TabModel* tabModel READ tabModel CONSTANT)
    Q_PROPERTY(int activeTabIndex READ activeTabIndex NOTIFY activeTabChanged)
    Q_PROPERTY(int tabCount READ tabCount NOTIFY activeTabChanged)
    // Open tabs with unsaved edits; the window shows the modified state while it is non-zero.
    Q_PROPERTY(int unsavedTabCount READ unsavedTabCount NOTIFY unsavedTabsChanged)

    Q_PROPERTY(QAbstractItemModel* explorerModel READ explorerModel CONSTANT)
    // Live fuzzy filter for the sidebar search; empty shows everything.
    Q_PROPERTY(QString explorerFilter READ explorerFilter WRITE setExplorerFilter NOTIFY
                   explorerFilterChanged)
    Q_PROPERTY(int operationCount READ operationCount NOTIFY projectChanged)
    Q_PROPERTY(int actorCount READ actorCount NOTIFY projectChanged)
    Q_PROPERTY(QString projectDefaultAuthLabel READ projectDefaultAuthLabel NOTIFY projectChanged)
    Q_PROPERTY(QStringList moduleNames READ moduleNames NOTIFY projectChanged)
    Q_PROPERTY(QStringList actorNames READ actorNames NOTIFY projectChanged)
    Q_PROPERTY(QStringList actorStrategies READ actorStrategies CONSTANT)
    Q_PROPERTY(QStringList operationIds READ operationIds NOTIFY projectChanged)

    // ── Actor detail (read-only panel in the centre pane) ───────────────────
    Q_PROPERTY(bool hasActor READ hasActor NOTIFY actorSelectionChanged)
    Q_PROPERTY(QString selectedActorName READ selectedActorName NOTIFY actorSelectionChanged)
    Q_PROPERTY(
        QString selectedActorDescription READ selectedActorDescription NOTIFY actorSelectionChanged)
    Q_PROPERTY(
        QString selectedActorStrategy READ selectedActorStrategy NOTIFY actorSelectionChanged)
    // The actor panel's edit copies of the scalar fields, parked per tab with the edited tables.
    Q_PROPERTY(bool actorEditing READ actorEditing NOTIFY actorEditFieldsChanged)
    Q_PROPERTY(QString actorEditName READ actorEditName WRITE setActorEditName NOTIFY
                   actorEditFieldsChanged)
    Q_PROPERTY(QString actorEditDescription READ actorEditDescription WRITE setActorEditDescription
                   NOTIFY actorEditFieldsChanged)
    Q_PROPERTY(QString actorEditStrategy READ actorEditStrategy WRITE setActorEditStrategy NOTIFY
                   actorEditFieldsChanged)
    Q_PROPERTY(bool actorEditRefreshEnabled READ actorEditRefreshEnabled WRITE
                   setActorEditRefreshEnabled NOTIFY actorEditFieldsChanged)
    Q_PROPERTY(QString actorEditRefreshMethod READ actorEditRefreshMethod WRITE
                   setActorEditRefreshMethod NOTIFY actorEditFieldsChanged)
    Q_PROPERTY(QString actorEditRefreshPath READ actorEditRefreshPath WRITE setActorEditRefreshPath
                   NOTIFY actorEditFieldsChanged)
    Q_PROPERTY(QString actorEditRefreshBody READ actorEditRefreshBody WRITE setActorEditRefreshBody
                   NOTIFY actorEditFieldsChanged)

    Q_PROPERTY(bool hasOperation READ hasOperation NOTIFY operationChanged)
    Q_PROPERTY(QString opName READ opName NOTIFY operationChanged)
    Q_PROPERTY(QString opMethod READ opMethod NOTIFY operationChanged)
    Q_PROPERTY(QString opPath READ opPath NOTIFY operationChanged)
    Q_PROPERTY(QString opActor READ opActor NOTIFY operationChanged)
    Q_PROPERTY(QString opBody READ opBody NOTIFY operationChanged)
    Q_PROPERTY(QStringList opDependencies READ opDependencies NOTIFY operationChanged)
    Q_PROPERTY(KeyValueModel* opHeaders READ opHeaders CONSTANT)
    Q_PROPERTY(KeyValueModel* opQuery READ opQuery CONSTANT)
    Q_PROPERTY(KeyValueModel* opExtractions READ opExtractions CONSTANT)
    Q_PROPERTY(KeyValueModel* opAssertions READ opAssertions CONSTANT)
    // Read-only headers the HTTP client adds automatically; values come from engine::HttpDefaults
    // so they never drift from the transport.
    Q_PROPERTY(QVariantList autoGeneratedHeaders READ autoGeneratedHeaders CONSTANT)

    // Environment + run state
    Q_PROPERTY(QStringList environments READ environments NOTIFY projectChanged)
    Q_PROPERTY(QString environment READ environment WRITE setEnvironment NOTIFY environmentChanged)
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(
        bool captureBodies READ captureBodies WRITE setCaptureBodies NOTIFY captureBodiesChanged)

    // Latest response
    Q_PROPERTY(bool hasResponse READ hasResponse NOTIFY responseChanged)
    Q_PROPERTY(int respStatus READ respStatus NOTIFY responseChanged)
    Q_PROPERTY(int respElapsedMs READ respElapsedMs NOTIFY responseChanged)
    Q_PROPERTY(int respBodySize READ respBodySize NOTIFY responseChanged)
    Q_PROPERTY(QString respHeaders READ respHeaders NOTIFY responseChanged)
    Q_PROPERTY(QString respBody READ respBody NOTIFY responseChanged)
    /// Parsed JSON tree of the response body, virtualized so large bodies render smoothly.
    Q_PROPERTY(ResponseBodyModel* responseBody READ responseBody CONSTANT)
    Q_PROPERTY(QString runOutcome READ runOutcome NOTIFY responseChanged)
    /// Name of the saved example currently shown (empty when showing a live run).
    Q_PROPERTY(QString shownExample READ shownExample NOTIFY responseChanged)

    // Live run timeline + saved examples for the open operation.
    Q_PROPERTY(TimelineModel* timeline READ timeline CONSTANT)
    Q_PROPERTY(ExampleListModel* examples READ examples CONSTANT)
    // Past runs (persisted history store), newest-first, for the history view.
    Q_PROPERTY(HistoryModel* history READ history CONSTANT)

    // ── Editable request surface ──
    // Seeded from the open operation on beginEdit; applyAndRun builds a one-shot RequestOverride,
    // saveOperation persists.
    Q_PROPERTY(bool editing READ editing NOTIFY editingChanged)
    Q_PROPERTY(bool creatingOperation READ creatingOperation NOTIFY editingChanged)
    Q_PROPERTY(bool newOperationDraftOpen READ newOperationDraftOpen NOTIFY activeTabChanged)
    Q_PROPERTY(QString newOperationModule READ newOperationModule NOTIFY editingChanged)
    Q_PROPERTY(QString newOperationDraftId READ newOperationDraftId NOTIFY editingChanged)
    Q_PROPERTY(
        QString newOperationName READ newOperationName WRITE setNewOperationName NOTIFY editChanged)
    Q_PROPERTY(QString editMethod READ editMethod WRITE setEditMethod NOTIFY editChanged)
    Q_PROPERTY(QString editPath READ editPath WRITE setEditPath NOTIFY editChanged)
    Q_PROPERTY(QString editActor READ editActor WRITE setEditActor NOTIFY editChanged)
    Q_PROPERTY(
        QString editExpectStatus READ editExpectStatus WRITE setEditExpectStatus NOTIFY editChanged)
    Q_PROPERTY(int editTimeout READ editTimeout WRITE setEditTimeout NOTIFY editChanged)
    Q_PROPERTY(bool editForce READ editForce WRITE setEditForce NOTIFY editChanged)
    Q_PROPERTY(QString editBody READ editBody WRITE setEditBody NOTIFY editChanged)
    Q_PROPERTY(bool editBodyIsForm READ editBodyIsForm WRITE setEditBodyIsForm NOTIFY editChanged)
    Q_PROPERTY(QString editBodyType READ editBodyType WRITE setEditBodyType NOTIFY editChanged)
    // The GraphQL editor's texts; each change rebuilds editBody while the variables are valid.
    Q_PROPERTY(
        QString editGraphqlQuery READ editGraphqlQuery WRITE setEditGraphqlQuery NOTIFY editChanged)
    Q_PROPERTY(QString editGraphqlVariables READ editGraphqlVariables WRITE setEditGraphqlVariables
                   NOTIFY editChanged)
    Q_PROPERTY(QString editGraphqlError READ editGraphqlError NOTIFY editChanged)
    // Inline (actor-less) auth. editAuthType is none/bearer/basic/apikey; the rest carry that
    // type's credential.
    Q_PROPERTY(QString editAuthType READ editAuthType WRITE setEditAuthType NOTIFY editChanged)
    Q_PROPERTY(QString editAuthToken READ editAuthToken WRITE setEditAuthToken NOTIFY editChanged)
    Q_PROPERTY(
        QString editAuthUsername READ editAuthUsername WRITE setEditAuthUsername NOTIFY editChanged)
    Q_PROPERTY(
        QString editAuthPassword READ editAuthPassword WRITE setEditAuthPassword NOTIFY editChanged)
    Q_PROPERTY(QString editAuthApiKeyName READ editAuthApiKeyName WRITE setEditAuthApiKeyName NOTIFY
                   editChanged)
    Q_PROPERTY(QString editAuthApiKeyValue READ editAuthApiKeyValue WRITE setEditAuthApiKeyValue
                   NOTIFY editChanged)
    Q_PROPERTY(bool editAuthApiKeyInQuery READ editAuthApiKeyInQuery WRITE setEditAuthApiKeyInQuery
                   NOTIFY editChanged)
    // AWS Signature v4 credentials.
    Q_PROPERTY(QString editAuthAwsAccessKey READ editAuthAwsAccessKey WRITE setEditAuthAwsAccessKey
                   NOTIFY editChanged)
    Q_PROPERTY(QString editAuthAwsSecretKey READ editAuthAwsSecretKey WRITE setEditAuthAwsSecretKey
                   NOTIFY editChanged)
    Q_PROPERTY(QString editAuthAwsRegion READ editAuthAwsRegion WRITE setEditAuthAwsRegion NOTIFY
                   editChanged)
    Q_PROPERTY(QString editAuthAwsService READ editAuthAwsService WRITE setEditAuthAwsService NOTIFY
                   editChanged)
    Q_PROPERTY(QString editAuthAwsSessionToken READ editAuthAwsSessionToken WRITE
                   setEditAuthAwsSessionToken NOTIFY editChanged)
    // OAuth 1.0a credentials.
    Q_PROPERTY(QString editAuthOauthConsumerKey READ editAuthOauthConsumerKey WRITE
                   setEditAuthOauthConsumerKey NOTIFY editChanged)
    Q_PROPERTY(QString editAuthOauthConsumerSecret READ editAuthOauthConsumerSecret WRITE
                   setEditAuthOauthConsumerSecret NOTIFY editChanged)
    Q_PROPERTY(QString editAuthOauthToken READ editAuthOauthToken WRITE setEditAuthOauthToken NOTIFY
                   editChanged)
    Q_PROPERTY(QString editAuthOauthTokenSecret READ editAuthOauthTokenSecret WRITE
                   setEditAuthOauthTokenSecret NOTIFY editChanged)
    // OAuth 2.0.
    Q_PROPERTY(QString editAuthOauth2GrantType READ editAuthOauth2GrantType WRITE
                   setEditAuthOauth2GrantType NOTIFY editChanged)
    Q_PROPERTY(QString editAuthOauth2ClientAuth READ editAuthOauth2ClientAuth WRITE
                   setEditAuthOauth2ClientAuth NOTIFY editChanged)
    // Authorization Code (PKCE) — interactive.
    Q_PROPERTY(QString editAuthOauth2AuthUrl READ editAuthOauth2AuthUrl WRITE
                   setEditAuthOauth2AuthUrl NOTIFY editChanged)
    Q_PROPERTY(QString editAuthOauth2CallbackUrl READ editAuthOauth2CallbackUrl WRITE
                   setEditAuthOauth2CallbackUrl NOTIFY editChanged)
    Q_PROPERTY(QString editAuthOauth2PkceMethod READ editAuthOauth2PkceMethod WRITE
                   setEditAuthOauth2PkceMethod NOTIFY editChanged)
    /// True once an access token has been obtained via "Get New Token".
    Q_PROPERTY(bool editAuthOauth2HasToken READ editAuthOauth2HasToken NOTIFY editChanged)
    Q_PROPERTY(QString editAuthOauth2TokenUrl READ editAuthOauth2TokenUrl WRITE
                   setEditAuthOauth2TokenUrl NOTIFY editChanged)
    Q_PROPERTY(QString editAuthOauth2ClientId READ editAuthOauth2ClientId WRITE
                   setEditAuthOauth2ClientId NOTIFY editChanged)
    Q_PROPERTY(QString editAuthOauth2ClientSecret READ editAuthOauth2ClientSecret WRITE
                   setEditAuthOauth2ClientSecret NOTIFY editChanged)
    Q_PROPERTY(QString editAuthOauth2Scope READ editAuthOauth2Scope WRITE setEditAuthOauth2Scope
                   NOTIFY editChanged)
    // JWT Bearer.
    Q_PROPERTY(QString editAuthJwtAlgorithm READ editAuthJwtAlgorithm WRITE setEditAuthJwtAlgorithm
                   NOTIFY editChanged)
    Q_PROPERTY(QString editAuthJwtSecret READ editAuthJwtSecret WRITE setEditAuthJwtSecret NOTIFY
                   editChanged)
    Q_PROPERTY(QString editAuthJwtPayload READ editAuthJwtPayload WRITE setEditAuthJwtPayload NOTIFY
                   editChanged)
    // Mutual TLS.
    Q_PROPERTY(QString editAuthMtlsCertPath READ editAuthMtlsCertPath WRITE setEditAuthMtlsCertPath
                   NOTIFY editChanged)
    Q_PROPERTY(QString editAuthMtlsKeyPath READ editAuthMtlsKeyPath WRITE setEditAuthMtlsKeyPath
                   NOTIFY editChanged)
    Q_PROPERTY(QString editAuthMtlsKeyPassword READ editAuthMtlsKeyPassword WRITE
                   setEditAuthMtlsKeyPassword NOTIFY editChanged)
    Q_PROPERTY(QString editAuthMtlsFormat READ editAuthMtlsFormat WRITE setEditAuthMtlsFormat NOTIFY
                   editChanged)
    Q_PROPERTY(QString editAuthMtlsCaCertPath READ editAuthMtlsCaCertPath WRITE
                   setEditAuthMtlsCaCertPath NOTIFY editChanged)
    Q_PROPERTY(EditableKeyValueModel* editHeaders READ editHeaders CONSTANT)
    Q_PROPERTY(EditableKeyValueModel* editQuery READ editQuery CONSTANT)
    Q_PROPERTY(EditableKeyValueModel* editForm READ editForm CONSTANT)
    Q_PROPERTY(EditableKeyValueModel* editActorConfig READ editActorConfig CONSTANT)
    // Actor auth config as a key→value map; reactive so fields refresh on prepareEdit/NewActor.
    Q_PROPERTY(QVariantMap actorConfig READ actorConfigMap NOTIFY actorEditChanged)
    Q_PROPERTY(EditableKeyValueModel* editEnvVars READ editEnvVars CONSTANT)
    Q_PROPERTY(QString editEnvBaseUrl READ editEnvBaseUrl WRITE setEditEnvBaseUrl NOTIFY
                   editEnvBaseUrlChanged)
    // Actor auth-endpoint editor seeds (read-only); the panel passes edits back to saveActorInline.
    Q_PROPERTY(QString actorAuthMethod READ actorAuthMethod NOTIFY actorEditChanged)
    Q_PROPERTY(QString actorAuthPath READ actorAuthPath NOTIFY actorEditChanged)
    Q_PROPERTY(QString actorAuthBody READ actorAuthBody NOTIFY actorEditChanged)
    Q_PROPERTY(QString actorAuthExpect READ actorAuthExpect NOTIFY actorEditChanged)
    Q_PROPERTY(bool actorHasRefresh READ actorHasRefresh NOTIFY actorEditChanged)
    Q_PROPERTY(QString actorRefreshMethod READ actorRefreshMethod NOTIFY actorEditChanged)
    Q_PROPERTY(QString actorRefreshPath READ actorRefreshPath NOTIFY actorEditChanged)
    Q_PROPERTY(QString actorRefreshBody READ actorRefreshBody NOTIFY actorEditChanged)
    Q_PROPERTY(EditableKeyValueModel* actorAuthExtract READ actorAuthExtract CONSTANT)
    Q_PROPERTY(EditableKeyValueModel* actorRefreshExtract READ actorRefreshExtract CONSTANT)
    // N-step login chain editor, one row per auth step; read back on saveActorInline.
    Q_PROPERTY(AuthStepListModel* actorAuthSteps READ actorAuthSteps CONSTANT)
    Q_PROPERTY(EditableKeyValueModel* editExtractions READ editExtractions CONSTANT)
    Q_PROPERTY(EditableKeyValueModel* editAssertions READ editAssertions CONSTANT)
    Q_PROPERTY(DependencyEditModel* editDependencies READ editDependencies CONSTANT)
    Q_PROPERTY(
        QStringList editDependencyCandidates READ editDependencyCandidates NOTIFY editChanged)
    // Live per-tab count badges (Postman-style "Headers 8").
    Q_PROPERTY(int editParamsCount READ editParamsCount NOTIFY editChanged)
    Q_PROPERTY(int editHeadersCount READ editHeadersCount NOTIFY editChanged)
    Q_PROPERTY(bool editBodyFilled READ editBodyFilled NOTIFY editChanged)
    Q_PROPERTY(int editChainCount READ editChainCount NOTIFY editChanged)
    Q_PROPERTY(int editAssertionsCount READ editAssertionsCount NOTIFY editChanged)
    // Chain preview nodes ({operationId, method, isTarget}).
    Q_PROPERTY(QVariantList chainNodes READ chainNodes NOTIFY chainChanged)
    // Layered DAG of the target's dependency chain, with node coordinates and edges.
    Q_PROPERTY(QVariantMap chainGraph READ chainGraph NOTIFY chainChanged)
    // Why the target's chain can't be resolved (a cycle, an ambiguous producer), or empty; the
    // chain graph and execution preview are empty while it is set.
    Q_PROPERTY(QString chainResolveError READ chainResolveError NOTIFY chainChanged)
    // Saved projects that could not be opened at startup or on retry; kept until removed.
    Q_PROPERTY(QVariantList unavailableProjects READ unavailableProjects NOTIFY openProjectsChanged)
    // Non-fatal findings from loading the active project (unknown keys, empty imports), each
    // prefixed with `file:line:column:`.
    Q_PROPERTY(QStringList loadWarnings READ loadWarnings NOTIFY projectChanged)
    // operationId → status token, so the chain graph colours nodes as a run streams.
    Q_PROPERTY(QVariantMap chainStatus READ chainStatus NOTIFY chainStatusChanged)
    // Whole-chain editor: every op in the target's chain, each with editable depends_on + extract.
    Q_PROPERTY(ChainEditorModel* chainEditor READ chainEditor CONSTANT)

public:
    explicit AppController(QObject* parent = nullptr);
    ~AppController() override;

    AppController(const AppController&) = delete;
    AppController& operator=(const AppController&) = delete;
    AppController(AppController&&) = delete;
    AppController& operator=(AppController&&) = delete;

    static AppController* create(QQmlEngine*, QJSEngine*) { return new AppController; }

    [[nodiscard]] QString projectName() const { return projectName_; }
    [[nodiscard]] QString projectRoot() const;
    [[nodiscard]] int resourceCount() const;
    [[nodiscard]] int latencySloP95Ms() const;
    [[nodiscard]] QString status() const { return status_; }
    /// Raw pointer to the current ProjectModel for C++ consumers; never a QML property.
    [[nodiscard]] const ProjectModel* projectRaw() const noexcept { return &activeProject(); }
    [[nodiscard]] ResourceListModel* resources() { return &resources_; }
    [[nodiscard]] OperationListModel* operations() { return &operations_; }
    [[nodiscard]] QString selectedModule() const { return selectedModule_; }

    // ── Tabs ─────────────────────────────────────────────────────────────
    [[nodiscard]] TabModel* tabModel() { return &tabs_; }
    [[nodiscard]] int activeTabIndex() const { return activeTabIndex_; }
    [[nodiscard]] int tabCount() const { return tabs_.count(); }
    [[nodiscard]] int unsavedTabCount() const { return tabs_.dirtyCount(); }
    /// Make the tab at `index` active, swapping its buffer into the live models.
    Q_INVOKABLE void activateTab(int index);
    /// Move the tab at `from` to `to`; the active tab follows.
    Q_INVOKABLE void moveTab(int from, int to);
    /// Close the tab; with unsaved edits it first asks Save, Discard or Cancel.
    Q_INVOKABLE void closeTab(int index);
    /// Close every tab except `index`, asking first when that drops unsaved edits.
    Q_INVOKABLE void closeOtherTabs(int index);
    /// The `hostApprovalRequested` id of an OAuth2 authorization waiting for approval; run
    /// questions use positive ids.
    static constexpr int kOauthApprovalId = -1;
    /// Answer a `hostApprovalRequested` question: true approves the host for this project and
    /// sends, false sends nothing.
    Q_INVOKABLE void answerHostApproval(int id, bool approve);
    /// Answer the pending `unsavedChangesPrompt`: 0 saves, 1 discards, anything else cancels.
    Q_INVOKABLE void resolveUnsavedChanges(int choice);
    /// True when the window may close now; otherwise asks about the unsaved tabs and emits
    /// `quitApproved` once they are saved or discarded.
    Q_INVOKABLE bool requestQuit();

    [[nodiscard]] QAbstractItemModel* explorerModel() { return &treeFilter_; }
    [[nodiscard]] QString explorerFilter() const;
    void setExplorerFilter(const QString& text);
    [[nodiscard]] int operationCount() const;
    [[nodiscard]] int actorCount() const;
    [[nodiscard]] QStringList moduleNames() const;
    [[nodiscard]] QStringList actorNames() const;

    /// Human-readable auth strategy for `actorName`; empty actor → "No authentication".
    Q_INVOKABLE [[nodiscard]] QString actorAuthLabel(const QString& actorName) const;

    /// The selectable auth strategy labels for the actor editor's combo.
    [[nodiscard]] QStringList actorStrategies() const;
    /// Description of an existing actor (for seeding the editor on Edit).
    Q_INVOKABLE [[nodiscard]] QString actorDescription(const QString& actorId) const;
    /// Reset the actor editor's config table for a new actor.
    Q_INVOKABLE void prepareNewActor();
    /// Load an existing actor's config into the editor's config table.
    Q_INVOKABLE void prepareEditActor(const QString& actorId);

    /// Show an actor's read-only detail in the centre pane, clearing any open operation.
    Q_INVOKABLE void selectActor(const QString& projectRoot, const QString& actorId);
    /// Start a new actor in the inline panel, opening in edit mode. Requires an open project.
    Q_INVOKABLE void newActor();
    /// Select an existing actor and enter inline edit mode.
    Q_INVOKABLE void requestActorEdit(const QString& projectRoot, const QString& actorId);
    /// Put the active actor tab in edit mode, seeding the edit copies from the actor; a no-op while
    /// it is already editing, so its edits survive.
    Q_INVOKABLE void beginActorEdit();
    /// Drop the active actor tab's edits and return to the read view; a never-saved draft's tab
    /// closes.
    Q_INVOKABLE void discardActorEdits();
    /// Save the active actor tab's edits with saveActorInline. Returns false, keeping the edits and
    /// showing why, when the save is rejected.
    Q_INVOKABLE bool saveActorEdits();
    /// Save an actor edited inline. `originalId` empty → create, else edit (rename when `name`
    /// differs). Returns true on success; the panel stays in edit mode on failure.
    Q_INVOKABLE bool saveActorInline(const QString& originalId,
                                     const QString& name,
                                     const QString& strategyLabel,
                                     const QString& description,
                                     bool refreshEnabled,
                                     const QString& refreshMethod,
                                     const QString& refreshPath,
                                     const QString& refreshBody);
    /// Delete an actor and clear it from any operation that referenced it.
    Q_INVOKABLE void deleteActor(const QString& actorId);

    // ── Environment create / edit / delete ──────────────────────────────────
    /// Reset the environment editor's variable table for a new environment.
    Q_INVOKABLE void prepareNewEnvironment();
    /// Load an existing environment's variables into the editor table.
    Q_INVOKABLE void prepareEditEnvironment(const QString& name);
    /// Create or update an environment (`originalName` empty → create). Returns true on success so
    /// the dialog stays open on rejection.
    Q_INVOKABLE bool saveEnvironmentEdits(const QString& originalName, const QString& name);
    /// Delete an environment.
    Q_INVOKABLE void deleteEnvironment(const QString& name);
    /// Whether the environment editor, with its name field holding `name`, differs from what was
    /// loaded by the last prepare or save.
    Q_INVOKABLE [[nodiscard]] bool environmentEditsChanged(const QString& name) const;
    /// The unsaved-changes dialog texts (title, message, saveText, discardText, cancelText) for the
    /// environment `title`. `reason`: 0 closes the dialog, 1 opens another environment, 2 quits.
    Q_INVOKABLE [[nodiscard]] QVariantMap environmentUnsavedPrompt(const QString& title,
                                                                   int reason) const;

    /// Set the project's p95 latency SLO budget in ms; 0 clears it.
    Q_INVOKABLE void setLatencySlo(int ms);

    /// Load / persist the expanded explorer-tree node keys (see ProjectTreeFilterModel::nodeKey);
    /// QSettings, app-global.
    [[nodiscard]] Q_INVOKABLE QStringList loadTreeExpansion() const;
    Q_INVOKABLE void saveTreeExpansion(const QStringList& keys) const;

    /// Reload the history view from the run-history store, newest first.
    Q_INVOKABLE void refreshHistory();

    /// Replay a past run into the timeline of its operation's tab (opened or focused first).
    /// Refused while that tab has a run in flight.
    Q_INVOKABLE void replayRun(qulonglong runId);

    /// Delete all runs for the current project's history, then refresh the view. Refused while a
    /// run is in flight.
    Q_INVOKABLE void clearHistory();

    [[nodiscard]] QString actorAuthMethod() const { return actorAuthMethod_; }
    [[nodiscard]] QString actorAuthPath() const { return actorAuthPath_; }
    [[nodiscard]] QString actorAuthBody() const { return actorAuthBody_; }
    [[nodiscard]] QString actorAuthExpect() const { return actorAuthExpect_; }
    [[nodiscard]] bool actorHasRefresh() const { return actorHasRefresh_; }
    [[nodiscard]] QString actorRefreshMethod() const { return actorRefreshMethod_; }
    [[nodiscard]] QString actorRefreshPath() const { return actorRefreshPath_; }
    [[nodiscard]] QString actorRefreshBody() const { return actorRefreshBody_; }
    [[nodiscard]] EditableKeyValueModel* actorAuthExtract() { return &actorAuthExtract_; }
    [[nodiscard]] EditableKeyValueModel* actorRefreshExtract() { return &actorRefreshExtract_; }
    [[nodiscard]] AuthStepListModel* actorAuthSteps() { return &actorAuthSteps_; }
    [[nodiscard]] QStringList operationIds() const;
    Q_INVOKABLE [[nodiscard]] QString operationMethod(const QString& operationId) const;

    [[nodiscard]] bool hasActor() const { return hasActor_; }
    [[nodiscard]] QString selectedActorName() const { return selectedActorName_; }
    [[nodiscard]] QString selectedActorDescription() const { return selectedActorDescription_; }
    [[nodiscard]] QString selectedActorStrategy() const { return selectedActorStrategy_; }
    [[nodiscard]] bool actorEditing() const { return actorEditing_; }
    [[nodiscard]] QString actorEditName() const { return actorEditName_; }
    [[nodiscard]] QString actorEditDescription() const { return actorEditDescription_; }
    [[nodiscard]] QString actorEditStrategy() const { return actorEditStrategy_; }
    [[nodiscard]] bool actorEditRefreshEnabled() const { return actorEditRefreshEnabled_; }
    [[nodiscard]] QString actorEditRefreshMethod() const { return actorEditRefreshMethod_; }
    [[nodiscard]] QString actorEditRefreshPath() const { return actorEditRefreshPath_; }
    [[nodiscard]] QString actorEditRefreshBody() const { return actorEditRefreshBody_; }
    void setActorEditName(const QString& value);
    void setActorEditDescription(const QString& value);
    void setActorEditStrategy(const QString& value);
    void setActorEditRefreshEnabled(bool value);
    void setActorEditRefreshMethod(const QString& value);
    void setActorEditRefreshPath(const QString& value);
    void setActorEditRefreshBody(const QString& value);

    [[nodiscard]] bool hasOperation() const { return hasOperation_; }
    [[nodiscard]] QString opName() const { return opName_; }
    [[nodiscard]] QString opMethod() const { return opMethod_; }
    [[nodiscard]] QString opPath() const { return opPath_; }
    [[nodiscard]] QString opActor() const { return opActor_; }
    [[nodiscard]] QString opBody() const { return opBody_; }
    [[nodiscard]] QStringList opDependencies() const { return opDependencies_; }
    [[nodiscard]] KeyValueModel* opHeaders() { return &opHeaders_; }
    [[nodiscard]] KeyValueModel* opQuery() { return &opQuery_; }
    [[nodiscard]] KeyValueModel* opExtractions() { return &opExtractions_; }
    [[nodiscard]] KeyValueModel* opAssertions() { return &opAssertions_; }
    [[nodiscard]] QVariantList autoGeneratedHeaders() const;

    [[nodiscard]] QStringList environments() const { return environments_; }
    [[nodiscard]] QString environment() const { return environment_; }
    void setEnvironment(const QString& env);
    [[nodiscard]] bool running() const { return running_; }
    [[nodiscard]] bool captureBodies() const { return captureBodies_; }
    void setCaptureBodies(bool capture);

    [[nodiscard]] bool hasResponse() const { return hasResponse_; }
    [[nodiscard]] int respStatus() const { return respStatus_; }
    [[nodiscard]] int respElapsedMs() const { return respElapsedMs_; }
    [[nodiscard]] int respBodySize() const { return respBodySize_; }
    [[nodiscard]] QString respHeaders() const { return respHeaders_; }
    [[nodiscard]] QString respBody() const { return respBody_; }
    [[nodiscard]] ResponseBodyModel* responseBody() { return &responseBody_; }
    [[nodiscard]] QString runOutcome() const { return runOutcome_; }
    [[nodiscard]] QString shownExample() const { return shownExample_; }
    [[nodiscard]] TimelineModel* timeline() { return &timeline_; }
    [[nodiscard]] ExampleListModel* examples() { return &exampleList_; }
    [[nodiscard]] HistoryModel* history() { return &history_; }

    // ── Editable request surface accessors ────────────────────────────────
    [[nodiscard]] bool editing() const { return editing_; }
    [[nodiscard]] bool creatingOperation() const { return creatingOperation_; }
    [[nodiscard]] bool newOperationDraftOpen() const { return operationDraftIndex() >= 0; }
    [[nodiscard]] QString newOperationModule() const { return newOperationModule_; }
    [[nodiscard]] QString newOperationDraftId() const { return newOperationDraftId_; }
    [[nodiscard]] QString newOperationName() const { return newOperationName_; }
    [[nodiscard]] QString editMethod() const { return editMethod_; }
    [[nodiscard]] QString editPath() const { return editPath_; }
    [[nodiscard]] QString editActor() const { return editActor_; }
    [[nodiscard]] QString editExpectStatus() const { return editExpectStatus_; }
    [[nodiscard]] int editTimeout() const { return editTimeout_; }
    [[nodiscard]] bool editForce() const { return editForce_; }
    [[nodiscard]] QString editBody() const { return editBody_; }
    [[nodiscard]] bool editBodyIsForm() const { return editBodyIsForm_; }
    [[nodiscard]] QString editBodyType() const { return editBodyType_; }
    [[nodiscard]] QString editGraphqlQuery() const { return editGraphqlQuery_; }
    [[nodiscard]] QString editGraphqlVariables() const { return editGraphqlVariables_; }
    [[nodiscard]] QString editGraphqlError() const;
    [[nodiscard]] QString editAuthType() const { return editAuthType_; }
    [[nodiscard]] QString editAuthToken() const { return editAuthToken_; }
    [[nodiscard]] QString editAuthUsername() const { return editAuthUsername_; }
    [[nodiscard]] QString editAuthPassword() const { return editAuthPassword_; }
    [[nodiscard]] QString editAuthApiKeyName() const { return editAuthApiKeyName_; }
    [[nodiscard]] QString editAuthApiKeyValue() const { return editAuthApiKeyValue_; }
    [[nodiscard]] bool editAuthApiKeyInQuery() const { return editAuthApiKeyInQuery_; }
    [[nodiscard]] QString editAuthAwsAccessKey() const { return editAuthAwsAccessKey_; }
    [[nodiscard]] QString editAuthAwsSecretKey() const { return editAuthAwsSecretKey_; }
    [[nodiscard]] QString editAuthAwsRegion() const { return editAuthAwsRegion_; }
    [[nodiscard]] QString editAuthAwsService() const { return editAuthAwsService_; }
    [[nodiscard]] QString editAuthAwsSessionToken() const { return editAuthAwsSessionToken_; }
    [[nodiscard]] QString editAuthOauthConsumerKey() const { return editAuthOauthConsumerKey_; }
    [[nodiscard]] QString editAuthOauthConsumerSecret() const {
        return editAuthOauthConsumerSecret_;
    }
    [[nodiscard]] QString editAuthOauthToken() const { return editAuthOauthToken_; }
    [[nodiscard]] QString editAuthOauthTokenSecret() const { return editAuthOauthTokenSecret_; }
    [[nodiscard]] QString editAuthOauth2GrantType() const { return editAuthOauth2GrantType_; }
    [[nodiscard]] QString editAuthOauth2ClientAuth() const { return editAuthOauth2ClientAuth_; }
    [[nodiscard]] QString editAuthOauth2AuthUrl() const { return editAuthOauth2AuthUrl_; }
    [[nodiscard]] QString editAuthOauth2CallbackUrl() const { return editAuthOauth2CallbackUrl_; }
    [[nodiscard]] QString editAuthOauth2PkceMethod() const { return editAuthOauth2PkceMethod_; }
    [[nodiscard]] bool editAuthOauth2HasToken() const {
        return !editAuthOauth2AccessToken_.isEmpty();
    }
    [[nodiscard]] QString editAuthOauth2TokenUrl() const { return editAuthOauth2TokenUrl_; }
    [[nodiscard]] QString editAuthOauth2ClientId() const { return editAuthOauth2ClientId_; }
    [[nodiscard]] QString editAuthOauth2ClientSecret() const { return editAuthOauth2ClientSecret_; }
    [[nodiscard]] QString editAuthOauth2Scope() const { return editAuthOauth2Scope_; }
    [[nodiscard]] QString editAuthJwtAlgorithm() const { return editAuthJwtAlgorithm_; }
    [[nodiscard]] QString editAuthJwtSecret() const { return editAuthJwtSecret_; }
    [[nodiscard]] QString editAuthJwtPayload() const { return editAuthJwtPayload_; }
    [[nodiscard]] QString editAuthMtlsCertPath() const { return editAuthMtlsCertPath_; }
    [[nodiscard]] QString editAuthMtlsKeyPath() const { return editAuthMtlsKeyPath_; }
    [[nodiscard]] QString editAuthMtlsKeyPassword() const { return editAuthMtlsKeyPassword_; }
    [[nodiscard]] QString editAuthMtlsFormat() const { return editAuthMtlsFormat_; }
    [[nodiscard]] QString editAuthMtlsCaCertPath() const { return editAuthMtlsCaCertPath_; }
    void setNewOperationName(const QString& name);
    void setEditMethod(const QString& method);
    void setEditPath(const QString& path);
    void setEditActor(const QString& actor);
    void setEditExpectStatus(const QString& expectStatus);
    void setEditTimeout(int timeoutMs);
    void setEditForce(bool force);
    void setEditBody(const QString& body);
    void setEditBodyIsForm(bool isForm);
    /// Body kind: none, form-data, x-www-form-urlencoded, json, xml, text or graphql. Keeps
    /// editBodyIsForm in sync and sets the canonical Content-Type unless the user set a custom one.
    /// Switching to graphql splits the current body into the GraphQL editor's texts.
    void setEditBodyType(const QString& type);
    void setEditGraphqlQuery(const QString& query);
    void setEditGraphqlVariables(const QString& variables);
    void setEditAuthType(const QString& type);
    void setEditAuthToken(const QString& token);
    void setEditAuthUsername(const QString& username);
    void setEditAuthPassword(const QString& password);
    void setEditAuthApiKeyName(const QString& name);
    void setEditAuthApiKeyValue(const QString& value);
    void setEditAuthApiKeyInQuery(bool inQuery);
    void setEditAuthAwsAccessKey(const QString& value);
    void setEditAuthAwsSecretKey(const QString& value);
    void setEditAuthAwsRegion(const QString& value);
    void setEditAuthAwsService(const QString& value);
    void setEditAuthAwsSessionToken(const QString& value);
    void setEditAuthOauthConsumerKey(const QString& value);
    void setEditAuthOauthConsumerSecret(const QString& value);
    void setEditAuthOauthToken(const QString& value);
    void setEditAuthOauthTokenSecret(const QString& value);
    void setEditAuthOauth2GrantType(const QString& value);
    void setEditAuthOauth2ClientAuth(const QString& value);
    void setEditAuthOauth2AuthUrl(const QString& value);
    void setEditAuthOauth2CallbackUrl(const QString& value);
    void setEditAuthOauth2PkceMethod(const QString& value);
    /// Run the interactive Authorization Code + PKCE flow and store the obtained token for Bearer
    /// injection.
    Q_INVOKABLE void oauth2GetNewToken();
    void setEditAuthOauth2TokenUrl(const QString& value);
    void setEditAuthOauth2ClientId(const QString& value);
    void setEditAuthOauth2ClientSecret(const QString& value);
    void setEditAuthOauth2Scope(const QString& value);
    void setEditAuthJwtAlgorithm(const QString& value);
    void setEditAuthJwtSecret(const QString& value);
    void setEditAuthJwtPayload(const QString& value);
    void setEditAuthMtlsCertPath(const QString& value);
    void setEditAuthMtlsKeyPath(const QString& value);
    void setEditAuthMtlsKeyPassword(const QString& value);
    void setEditAuthMtlsFormat(const QString& value);
    void setEditAuthMtlsCaCertPath(const QString& value);
    /// Native file-open dialog; returns the chosen path (empty if cancelled). `nameFilter` uses
    /// Qt's filter syntax.
    Q_INVOKABLE [[nodiscard]] QString pickFile(const QString& title,
                                               const QString& nameFilter) const;
    [[nodiscard]] EditableKeyValueModel* editHeaders() { return &editHeaders_; }
    [[nodiscard]] EditableKeyValueModel* editQuery() { return &editQuery_; }
    [[nodiscard]] EditableKeyValueModel* editForm() { return &editForm_; }
    [[nodiscard]] EditableKeyValueModel* editActorConfig() { return &actorConfig_; }
    [[nodiscard]] QVariantMap actorConfigMap() const;
    /// Upsert one auth-config key (empty value clears it).
    Q_INVOKABLE void setActorConfigValue(const QString& key, const QString& value);
    [[nodiscard]] EditableKeyValueModel* editEnvVars() { return &envVars_; }
    [[nodiscard]] QString editEnvBaseUrl() const { return editEnvBaseUrl_; }
    void setEditEnvBaseUrl(const QString& url);
    [[nodiscard]] EditableKeyValueModel* editExtractions() { return &editExtractions_; }
    [[nodiscard]] DependencyEditModel* editDependencies() { return &editDependencies_; }
    [[nodiscard]] EditableKeyValueModel* editAssertions() { return &editAssertions_; }
    [[nodiscard]] QStringList editDependencyCandidates() const;
    [[nodiscard]] int editParamsCount() const;
    [[nodiscard]] int editHeadersCount() const;
    [[nodiscard]] bool editBodyFilled() const;
    [[nodiscard]] int editChainCount() const;
    [[nodiscard]] int editAssertionsCount() const;
    [[nodiscard]] QVariantList chainNodes() const;
    [[nodiscard]] QVariantMap chainGraph() const;
    [[nodiscard]] QString chainResolveError() const;
    [[nodiscard]] QStringList loadWarnings() const;
    [[nodiscard]] Q_INVOKABLE QVariantList executionPreview() const;
    [[nodiscard]] Q_INVOKABLE QStringList extractionConsumers(const QString& producerOperationId,
                                                              const QString& variable) const;
    [[nodiscard]] QVariantMap chainStatus() const;
    [[nodiscard]] ChainEditorModel* chainEditor() { return &chainEditor_; }

    /// Rebuild the whole-chain editor from the open operation's transitive dependency chain. Call
    /// when the Chain tab is shown.
    Q_INVOKABLE void prepareChainEditor();
    /// Re-derive chain editor membership from the current (possibly unsaved) dependency edits,
    /// preserving in-progress step edits.
    Q_INVOKABLE void syncChainEditorMembership();

    /// Add `operationId` as a dependency of the chain's target, then re-derive membership.
    Q_INVOKABLE void chainAddDependency(const QString& operationId);
    /// Remove `operationId` from every step's dependencies, then re-derive membership.
    Q_INVOKABLE void chainRemoveStep(const QString& operationId);
    /// Persist every chain operation's edited depends_on + extract in one validated write. Returns
    /// true on success.
    Q_INVOKABLE bool saveChainEdits();

    /// The `{{resource.variable}}` tokens an operation extracts, shown under a dependency.
    Q_INVOKABLE [[nodiscard]] QStringList extractedVariablesFor(const QString& operationId) const;

    /// Variable/path pairs an operation extracts, as {key, value} (bare name, source path).
    Q_INVOKABLE [[nodiscard]] QVariantList extractionPairsFor(const QString& operationId) const;

    /// Variables referenceable at `operationId` for `{{` autocomplete, as {token, kind, detail}
    /// maps. Empty if the chain can't be resolved or no project is open.
    Q_INVOKABLE [[nodiscard]] QVariantList variableSuggestions(const QString& operationId) const;

    /// Candidate values for a `{{resource.var}}` token from the producing op's saved examples.
    /// Empty when there is no producer or example.
    Q_INVOKABLE [[nodiscard]] QStringList candidateValues(const QString& token) const;

    /// The operation id that extracts `{{resource.var}}`, or empty if none does.
    Q_INVOKABLE [[nodiscard]] QString producerOpFor(const QString& token) const;

    /// Pin or clear (empty `value`) a concrete value for a `{{resource.var}}` token, applied to
    /// every later run. Changing a pin resets the extraction cache.
    Q_INVOKABLE void setVariableOverride(const QString& token, const QString& value);
    /// The currently pinned value for `token`, or empty if none.
    Q_INVOKABLE [[nodiscard]] QString variableOverride(const QString& token) const;

    /// Run the producing (list) endpoint for `token`, auto-save its response as an example and
    /// refresh the picker. No-op while a run is in flight.
    Q_INVOKABLE void refreshCandidates(const QString& token);

    /// The live, editable extract model for a step in the chain editor, or nullptr if not in the
    /// chain. Persisted by saveChainEdits.
    Q_INVOKABLE [[nodiscard]] EditableKeyValueModel* chainExtractModelFor(
        const QString& operationId);

    /// Resources the step can fan out over (the other steps' resources), for the for-each combo.
    Q_INVOKABLE [[nodiscard]] QStringList chainForEachOptions(const QString& operationId) const;

    /// The resource the given chain step currently iterates ("" = run once).
    Q_INVOKABLE [[nodiscard]] QString chainForEachOver(const QString& operationId) const;

    /// Set (or clear, when empty) the for-each target of a chain step.
    Q_INVOKABLE void chainSetForEach(const QString& operationId, const QString& overResource);

    /// Whether the given chain step keeps running after a failed iteration.
    Q_INVOKABLE [[nodiscard]] bool chainForEachContinueOnError(const QString& operationId) const;

    /// Set the for-each continue-on-error flag of a chain step.
    Q_INVOKABLE void chainSetForEachContinueOnError(const QString& operationId,
                                                    bool continueOnError);

    /// Validate an extraction path against the step's response (live body for the open op, else its
    /// latest example). Returns {state: "match"|"nomatch"|"neutral", value}; "neutral" when there
    /// is nothing to check.
    Q_INVOKABLE [[nodiscard]] QVariantMap evaluateExtractionPath(const QString& operationId,
                                                                 const QString& path) const;

    /// Leaf JSONPaths in the step's response matching `prefix` (case-insensitive; empty = all).
    /// Empty when there is no response.
    Q_INVOKABLE [[nodiscard]] QStringList suggestExtractionPaths(const QString& operationId,
                                                                 const QString& prefix) const;

    /// Save a value from the current response as an extracted variable (`sourcePath` is a
    /// JSONPath). Persists immediately; returns true on success.
    Q_INVOKABLE bool addExtraction(const QString& variableName, const QString& sourcePath);

    /// Evaluate a predicate `expression` against the latest response using the engine's grammar.
    /// Returns {valid, passed, error}; `valid==false` means malformed. Gate the UI on
    /// `hasResponse`.
    Q_INVOKABLE [[nodiscard]] QVariantMap evaluateAssertion(const QString& expression) const;

    /// Preview how the edit-mode form-data body would be encoded, as {valid, error, multipart,
    /// contentType, totalBytes, parts}. Reads referenced files, so call on demand, not per
    /// keystroke.
    Q_INVOKABLE [[nodiscard]] QVariantMap previewFormBody() const;

    /// Per-actor cookie jars from the current run context, as {actor, cookies:[{name,value}]};
    /// empty jars omitted.
    Q_INVOKABLE [[nodiscard]] QVariantList cookieJars() const;

    /// Open a project directory (the folder containing reqloom.yaml).
    Q_INVOKABLE void openProject(const QUrl& directory);

    /// Switch to a known project by absolute directory path (used by the workspace switcher).
    Q_INVOKABLE void openProjectPath(const QString& path);

    /// Recent projects, newest first, as {name, path} maps; entries with no reqloom.yaml are
    /// filtered out.
    Q_INVOKABLE [[nodiscard]] QVariantList recentProjects() const;

    /// Drop a project from the workspace switcher list (does not touch disk).
    Q_INVOKABLE void removeRecentProject(const QString& path);

    /// Open projects as {name, path, index, active} maps in workspace order.
    Q_INVOKABLE [[nodiscard]] QVariantList openProjects();

    /// Saved projects that could not be opened, as {path, name, reason} maps.
    [[nodiscard]] QVariantList unavailableProjects() const;
    /// Try to open an unavailable saved project again; it stays listed with the new reason if
    /// it still fails.
    Q_INVOKABLE void retryUnavailableProject(const QString& path);
    /// Remove an unavailable project from the saved workspace (does not touch disk).
    Q_INVOKABLE void removeUnavailableProject(const QString& path);

    /// Switch the active project to the open one at `index`. No-op while a run is in flight.
    Q_INVOKABLE void activateProject(int index);

    /// Close the open project at `index` (nothing on disk is deleted). No-op while a run is in
    /// flight on it.
    Q_INVOKABLE void closeProject(int index);

    /// Close the open collection identified by its root path.
    Q_INVOKABLE void closeProjectByRoot(const QString& projectRoot);

    /// Import an OpenAPI 3.x document into a project written to `targetDir`, then load it. Parse +
    /// write run off the GUI thread; results arrive via `notify` and `importReviewNotes`. If
    /// `targetDir` already holds a project and `overwrite` is false, emits `importNeedsOverwrite`.
    Q_INVOKABLE void importOpenApi(const QUrl& specFile, const QUrl& targetDir, bool overwrite);

    /// Select a module by id; populates `operations` with its endpoints.
    Q_INVOKABLE void selectModule(const QString& moduleName);

    /// Open an endpoint in the editor (populates the op* properties/models).
    Q_INVOKABLE void selectOperation(const QString& moduleName, const QString& opName);
    /// Close the editor and return to the endpoint list.
    Q_INVOKABLE void closeOperation();

    /// Run the open operation end-to-end (resolves + executes its chain).
    /// `dryRun` previews without sending; `clean` resets caches first.
    Q_INVOKABLE void runSelected(bool clean, bool dryRun);

    // ── Edit mode ──
    /// Turn on Edit mode: seed every editable control from the open operation.
    Q_INVOKABLE void beginEdit();
    /// Leave Edit mode without applying (read preview returns).
    Q_INVOKABLE void cancelEdit();
    /// Run with the current edits as a one-shot RequestOverride; the loaded project is never
    /// mutated.
    Q_INVOKABLE void applyAndRun(bool clean, bool dryRun);
    /// Persist the edits: patch the real operation, then ProjectModel::saveOperation, which
    /// validates (a cycle or undefined op is rejected with a notify()).
    Q_INVOKABLE void saveOperation();

    /// Persist the edit-auth fields as the project-wide default (used by "Inherit from parent").
    Q_INVOKABLE void saveProjectDefaultAuth();

    /// Label for the project default auth type. A property so the Inherit hint refreshes when the
    /// project changes.
    [[nodiscard]] QString projectDefaultAuthLabel() const;

    /// Open the standalone hook-script editor (a Widgets dialog) for the current operation; edits
    /// persist on Save.
    Q_INVOKABLE void openHookEditor();

    /// Write `reqloom.d.ts` (the hook sandbox `ctx` types) into the project root and report the
    /// path, so a TypeScript-aware editor autocompletes in `./hooks/*.js`.
    Q_INVOKABLE void generateHookTypings();

    /// Cancel the in-flight run, if any.
    Q_INVOKABLE void cancelRun();
    /// Reset the run context's session + extraction caches (refused mid-run).
    Q_INVOKABLE void resetCaches();

    /// Save the displayed response as a named example for the open operation. No-op without a
    /// response or operation.
    Q_INVOKABLE void saveResponse(const QString& name);

    /// Show a saved example (by name) of the open operation in the response panel.
    Q_INVOKABLE void showExample(const QString& name);

    /// Body of a saved example of the open operation, or empty if none; used by the Diff tab.
    Q_INVOKABLE [[nodiscard]] QString exampleBody(const QString& name) const;

    /// LCS line diff of `oldText` → `newText` as [{sign, text}] rows; sign is "+", "-" or " "
    /// (context). Inputs too large to diff give one row {tooLarge: true, text: <message>}.
    Q_INVOKABLE [[nodiscard]] QVariantList lineDiff(const QString& oldText,
                                                    const QString& newText) const;

    /// Indent JSON two spaces per level, keeping number and string tokens exactly as written;
    /// other text comes back unchanged. `unwrapEncoded` formats a JSON string holding a document.
    Q_INVOKABLE [[nodiscard]] QString prettyJson(const QString& text,
                                                 bool unwrapEncoded = false) const;

    /// HTML (or Markdown) for the response preview with every image, background image and other
    /// loadable reference removed, so showing it fetches nothing.
    Q_INVOKABLE [[nodiscard]] QString inertPreviewHtml(const QString& source, bool markdown) const;

    /// Copy `text` to the clipboard and toast a confirmation (`label` names what was copied).
    Q_INVOKABLE void copyToClipboard(const QString& text, const QString& label);

    /// Convert a file-picker URL to a local path, for the form-data editor's `@<path>` upload
    /// convention.
    Q_INVOKABLE [[nodiscard]] QString localFileFromUrl(const QUrl& url) const;

    /// Form-field `@` rules, delegated to the engine (FormBody.h).
    Q_INVOKABLE [[nodiscard]] bool isFormFileValue(const QString& value) const;
    Q_INVOKABLE [[nodiscard]] QString formValueText(const QString& value) const;
    Q_INVOKABLE [[nodiscard]] QString formTextValue(const QString& text) const;
    /// The open operation's form body as it will be sent: [{key, text, isFile}], where `text` is
    /// the literal value or, for a file field, its path. Empty when the body is not a form.
    Q_INVOKABLE [[nodiscard]] QVariantList readBodyFormRows() const;

    // ── Explorer selection / activation by fully-qualified id ──────────────
    /// Select an operation row ("<resource>.<op>") and open it (select-to-preview).
    Q_INVOKABLE void selectOperationById(const QString& operationId);
    /// Open an operation by id and enter Edit mode; used by the chain graph.
    Q_INVOKABLE void editOperationById(const QString& operationId);
    /// Activate an operation row (double-click / Enter): open it and run it.
    Q_INVOKABLE void activateOperationById(const QString& operationId);

    // ── Multi-project selection (aggregated explorer tree) ───────────────────
    /// Select an operation in a specific open collection, activating that project first if needed.
    Q_INVOKABLE void selectOperationInProject(const QString& projectRoot,
                                              const QString& operationId);
    /// Like selectOperationInProject, then run it (double-click / Enter).
    Q_INVOKABLE void activateOperationInProject(const QString& projectRoot,
                                                const QString& operationId);
    /// Make the collection with `projectRoot` active without opening an operation. Returns false if
    /// it couldn't switch (unknown root or a run in flight); callers must then skip project-scoped
    /// actions.
    Q_INVOKABLE bool activateProjectByRoot(const QString& projectRoot);

    // ── Create / rename / delete (validated by the engine via ProjectModel; emit `notify`) ──
    /// Validate a name as ProjectModel does (the engine's file-stem rule plus no '.'), so dialogs
    /// can gate OK.
    Q_INVOKABLE [[nodiscard]] bool isValidName(const QString& name) const;

    /// Operations a delete would change, for the confirmation dialog: `kind` "operation" or
    /// "resource" lists those whose depends_on loses it; "actor" lists those that run as it.
    Q_INVOKABLE [[nodiscard]] QStringList dependentsOf(const QString& kind,
                                                       const QString& id) const;

    Q_INVOKABLE void createResource(const QString& name, const QString& description);

    /// Scaffold and open a new empty project named `name` in `directory` (the "New Project" entry
    /// point). Toasts the outcome.
    Q_INVOKABLE void createProject(const QUrl& directory, const QString& name);

    /// Start a transient endpoint draft for `preselectedResource`; not persisted until Save, and
    /// Cancel restores the previous tab.
    Q_INVOKABLE void prepareNewEndpoint(const QString& preselectedResource);

    Q_INVOKABLE void renameOperation(const QString& operationId, const QString& newName);
    Q_INVOKABLE void deleteOperation(const QString& operationId);
    Q_INVOKABLE void renameResource(const QString& resourceId, const QString& newName);
    Q_INVOKABLE void deleteResource(const QString& resourceId);

    // ── Saved examples (explorer child rows) ──
    Q_INVOKABLE void selectExample(const QString& operationId, const QString& exampleName);
    Q_INVOKABLE void renameExample(const QString& operationId,
                                   const QString& oldName,
                                   const QString& newName);
    Q_INVOKABLE void duplicateExample(const QString& operationId, const QString& exampleName);
    Q_INVOKABLE void deleteExample(const QString& operationId, const QString& exampleName);

signals:
    void projectChanged();
    /// Emitted when the explorer's live filter text changes.
    void explorerFilterChanged();
    /// Emitted when the recent-projects list changes.
    void recentProjectsChanged();
    /// Emitted when the set of open projects or the active one changes.
    void openProjectsChanged();
    void selectionChanged();
    void operationChanged();
    void environmentChanged();
    void runningChanged();
    void responseChanged();
    /// Fired after replayRun() rebuilt the timeline, so the UI can switch to it.
    void runReplayed();
    void captureBodiesChanged();
    void editingChanged();
    /// Fired on any editable-field or edit-model change.
    void editChanged();

    /// Fired after prepareNewActor/prepareEditActor seeds the actor editor.
    void actorEditChanged();
    /// Fired when the read-only actor detail selection changes.
    void actorSelectionChanged();
    /// Fired when actor edit mode or one of its edit copies changes.
    void actorEditFieldsChanged();
    /// Fired when the active tab or the tab set changes.
    void activeTabChanged();
    /// Fired after an endpoint draft's tab closed.
    void newOperationDraftDiscarded();
    /// Ask whether to save the unsaved edits an action would drop; answer with
    /// `resolveUnsavedChanges`.
    void unsavedChangesPrompt(
        QString title, QString message, QString saveText, QString discardText, QString cancelText);
    /// A run is about to send `credential` to `host`, which the person has not approved for this
    /// project (FR-15.9); the run waits for `answerHostApproval(id, …)`.
    void hostApprovalRequested(
        int id, QString projectName, QString host, QString credential, QString url);
    /// The unsaved tabs were saved or discarded after `requestQuit`; the window may close.
    void quitApproved();
    void unsavedTabsChanged();

    /// Fired when the env editor's Base URL field is seeded programmatically.
    void editEnvBaseUrlChanged();
    /// Fired when the chain preview needs to re-render.
    void chainChanged();
    /// Fired when a run streams a new per-operation status (chain graph colours).
    void chainStatusChanged();
    /// Fired when a value-picker pin is set or cleared.
    void variableOverridesChanged();
    /// Transient feedback for create/rename/delete outcomes; `isError` tints the message.
    void notify(QString message, bool isError);
    /// Something worth reading that is not a failure, such as load warnings; stays until dismissed.
    void notifyWarning(QString message);

    /// Emitted after an OpenAPI import when the engine produced per-operation review notes.
    void importReviewNotes(QString notes);

    /// Emitted when an import target already holds a reqloom.yaml and `overwrite` was not passed;
    /// carries the original arguments so the UI can confirm and re-invoke.
    void importNeedsOverwrite(QUrl specFile, QUrl targetDir);

    /// Fired after a run, when the cookie jars may have changed.
    void cookiesChanged();

    /// An operation of the active project got a new id (its rename or its module's); emitted
    /// before the project's `projectChanged`, after its saved examples moved to `newId`.
    void operationRenamed(QString oldId, QString newId);
    /// An operation of the active project was deleted (alone or with its module).
    void operationRemoved(QString operationId);

private:
    void onLoaded();
    void onSaved();
    void onLoadFailed(const QString& code, const QString& detail);
    void onOperationRenamed(const QString& oldId, const QString& newId);
    void onOperationRemoved(const QString& operationId);
    /// Fall back to the project's default environment when the selected one no longer exists.
    void ensureEnvironmentExists();
    void loadSampleIfPresent();
    /// Best-effort refresh of the project's `reqloom.d.ts` hook typings; failures are swallowed.
    void refreshHookTypings();
    /// Open (or switch to) the per-project run-history database, whose path derives from a hash of
    /// the project root.
    void openProjectHistory(const QString& projectRoot);
    /// The workspace's active project. Never null: the workspace always holds at least one project.
    [[nodiscard]] ProjectModel& activeProject() noexcept;
    [[nodiscard]] const ProjectModel& activeProject() const noexcept;
    /// Add the just-loaded project to the recent list (newest first, de-duplicated by path,
    /// capped). Emits `recentProjectsChanged`.
    void recordRecentProject(const QString& name, const QString& path);
    /// Bind a project's load/save/failure signals to this controller's slots.
    void bindProject(ProjectModel* project);
    /// Rebind every per-active-project view and the run controller. Shared by a fresh load and a
    /// plain switch.
    void rebindActiveProject(bool repopulateTree = true);
    /// Rebuild the aggregated explorer tree from every open collection.
    void populateWorkspaceTree();
    /// Select the active project's first module so the centre pane isn't empty.
    void selectFirstModule();
    /// Clear the read-only actor detail (an operation/module took the pane).
    void clearActorSelection();

    // ── Tab state snapshot / restore (multi-tab editor) ──
    /// Copy the live editor state into the active tab's buffer; no-op without an active tab.
    void captureActiveTab();
    /// Load the active tab's buffer into the live models and emit every change signal. An operation
    /// tab's read fields are re-derived from the project, then the edit + response snapshot is
    /// overlaid.
    void restoreActiveTab();
    /// Snapshot the live operation edit/response state into `tab` (read fields are re-derived on
    /// restore).
    void captureOperationInto(TabState& tab) const;
    /// Snapshot the live actor edit state into `tab`.
    void captureActorInto(TabState& tab) const;
    /// Load an operation's read fields from the project into the live op* state, without touching
    /// tabs or emitting. Returns false if the op no longer exists.
    bool loadOperationReadState(const QString& moduleName, const QString& opName);
    /// Restore the live edit-mode scalars + models + response from `tab`.
    void restoreOperationEditFrom(const TabState& tab);
    /// Restore the live actor edit models + scalars from `tab`.
    void restoreActorFrom(const TabState& tab);
    /// Rebuild editBody from the GraphQL editor's texts and emit the edit signals.
    void syncGraphqlBody();
    /// Open (or focus) an operation tab, snapshotting the outgoing tab first.
    void openOperationTab(const QString& projectRoot,
                          const QString& moduleName,
                          const QString& opName);
    /// Open (or focus) an actor tab; `isNewDraft` opens a blank unsaved actor in edit mode.
    void openActorTab(const QString& projectRoot, const QString& actorId, bool isNewDraft);
    /// Sync hasOperation_/hasActor_ from the active tab kind (both cleared with no tabs) and emit.
    void syncActiveKindFlags();
    /// Recompute the active tab's dirty flag and refresh its strip row if it changed.
    void updateActiveTabDirty();
    /// Queue one updateActiveTabDirty for the next event-loop turn.
    void scheduleActiveTabDirtyUpdate();
    /// Seed the actor edit copies from the selected actor's saved values.
    void seedActorEditFields();
    /// Leave actor edit mode and drop the active actor tab's baseline.
    void endActorEdit();
    /// Assign `value` to the actor edit copy `field`, emitting when it changed.
    template <typename T>
    void setActorEditField(T& field, const T& value);
    /// Start or drop the active tab's edit baseline as edit mode starts or ends.
    void onActiveTabEditingChanged();
    /// The live edits of the active operation tab, with its baseline, for the dirty check.
    [[nodiscard]] TabState liveOperationEdits() const;
    /// The chain fields were reseeded from a save: take them into the active tab's baseline.
    void rebaseChainEdits();
    /// Connect tab bookkeeping (dirty count, renamed/deleted operations, per-tab chain status).
    void wireTabTracking();
    void renameOperationInTabs(const QString& oldId, const QString& newId);
    void removeOperationFromTabs(const QString& operationId);
    /// Ask about the unsaved tabs among `closing`; `proceed` runs once they are saved or
    /// discarded. Returns false when none is unsaved (nothing was asked).
    bool askAboutUnsaved(const std::vector<int>& closing,
                         UnsavedReason reason,
                         std::function<void()> proceed);
    /// Activate and save the tab at `index` as the Save button would; false when it stays unsaved.
    bool saveTabAt(int index);
    /// Keep the unsaved edits of a tab whose operation no longer exists as an endpoint draft.
    void detachTab(int index);
    /// Make the collection with `projectRoot` active (no tree repopulate, no auto-select). Returns
    /// false if it couldn't switch.
    [[nodiscard]] bool activateForRow(const QString& projectRoot, std::function<void()> retry = {});
    /// Persist the open-project set and active project for restore on next launch.
    void persistOpenProjects();
    void persistActiveProject();
    void restoreOpenProjects();
    /// Persist the open editor tabs (kind, id, active index) per project root in QSettings.
    /// Only descriptors are stored, never edit buffers (which may hold secrets).
    void persistOpenTabs() const;
    void restoreOpenTabs(const QString& projectRoot);
    /// Refresh `exampleList_` for the open operation and rebuild the explorer's example child rows.
    void refreshExamples();
    /// Refresh only the open operation's example list; the explorer tree is untouched so selecting
    /// does not collapse it.
    void refreshOpenOpExamples();
    /// Point `exampleStore_` at the active project and import its legacy examples file once.
    void openExampleStore();
    /// Open folders an import must not write into now (a run in flight, unsaved edits), and why.
    [[nodiscard]] std::vector<BlockedFolder> importBlockedFolders() const;
    /// Load the project a successful import wrote (reloading it when already open), then report.
    void finishImport(const ImportOutcome& outcome);
    /// Push per-actor session state into the explorer tree and start/stop `sessionTicker_` so TTLs
    /// count down only while a session is live.
    void refreshActorSessions();
    /// Re-seed the edit-mode depends_on / extract models from the project, so a later endpoint Save
    /// can't clobber a Chain-tab save.
    void seedEditChainFromProject();
    /// The operation the engine would run to produce `token` ("<resource>.<var>") for the open
    /// request, unsaved edits included, or why there is none.
    [[nodiscard]] ProducerLookup findVariableProducer(const QString& token) const;
    /// Best response body to check a step's extraction against without a
    /// re-run: the live body when it's the open op, else its latest example.
    [[nodiscard]] QString responseBodyFor(const QString& operationId) const;
    /// Fully-qualified id of the open operation ("<module>.<op>"), or empty.
    [[nodiscard]] QString currentOperationId() const;
    /// True when the active project's tabs may close now; otherwise asks about their unsaved
    /// edits and runs `retry` once they are saved or discarded.
    [[nodiscard]] bool canLeaveActiveProject(const std::function<void()>& retry);
    /// Clear the active transient creation identity without touching its tab row.
    void clearNewOperationDraftIdentity();
    /// Remove a tab without prompting; shared by ordinary closes and confirmed discard.
    void closeTabImmediately(int index);
    /// Remove every tab except `index` without prompting.
    void closeOtherTabsImmediately(int index);
    /// Index of the one transient endpoint draft, or -1 when none exists.
    [[nodiscard]] int operationDraftIndex() const;
    /// Persist the transient draft once through ProjectModel's complete-operation path.
    void saveNewOperation();
    /// Assemble a one-shot RequestOverride from the current edit state.
    [[nodiscard]] RequestOverride buildOverride() const;

    /// Build an InlineAuth from the current edit-auth fields, or nullopt when
    /// the type is "none". Shared by buildOverride and saveProjectDefaultAuth.
    [[nodiscard]] std::optional<engine::InlineAuth> buildInlineAuthFromEdit() const;

    /// Set the Content-Type in editHeaders_ to `desired` only when the current one is empty or a
    /// managed canonical type.
    void setManagedContentType(const QString& desired);

    // ── Run routing (AppControllerRun.cpp): a run's events belong to the tab that started it ──
    void wireRunEvents();
    void wireRunLifecycle();
    void wireRunSteps();
    void wireRunResponses();
    void wireRunChecks();
    /// Store the refresh-candidates producer's 2xx body as its "Latest" example, once per arming.
    void saveCandidateExample(const QString& op, int status, const QString& body);
    /// A run was accepted from the shown tab: make it the run's tab and clear its response.
    void beginRunInActiveTab();
    /// The worker finished: show an engine refusal, park a hidden tab's timeline, end the run.
    void finishRun(const RunReport& report);
    [[nodiscard]] RunTabRouter::TabKey activeTabKey() const;
    /// Index of the run's tab, or -1 when it is gone or the run started with no tab open.
    [[nodiscard]] int runTabIndex() const;
    [[nodiscard]] bool runTabShown() const;
    /// Timeline for the run's next event, or nullptr when the event belongs nowhere.
    [[nodiscard]] TimelineModel* runTimeline();
    void applyTargetResponse(
        int status, const QString& headers, int bodySize, qint64 elapsedMs, const QString& body);
    void applyRunOutcome(const QString& outcome);
    /// Cancel the in-flight run once the tab that started it is no longer open.
    void cancelRunIfItsTabClosed();
    /// About to show the run's tab: park what it gathered while hidden in the tab's snapshot.
    void parkHiddenRunTimeline();
    /// Open or focus the tab of a replayed run's operation, so a replay never lands in another
    /// operation's tab; false (after telling the person why) when the replay must not run.
    [[nodiscard]] bool focusReplayTab(const std::vector<engine::RunEvent>& events);

    std::unique_ptr<WorkspaceModel> workspace_;
    std::unique_ptr<Bootstrapper> bootstrapper_;
    std::unique_ptr<RunController> runController_;

    /// Delivers the off-thread import result (`runImportJob`) back to the GUI thread.
    QFutureWatcher<ImportOutcome> importWatcher_;
    SavedResponseStore exampleStore_;
    /// Project roots whose unreadable legacy examples file was already reported this session.
    QSet<QString> legacyExamplesReported_;
    ResourceListModel resources_;
    OperationListModel operations_;
    ProjectTreeModel tree_;
    // Re-reads actor session TTLs while some actor holds a live session. A 15s poll, so the dot can
    // lag by up to 15s; the upgrade is a RunContext state-change signal.
    QTimer sessionTicker_;
    ProjectTreeFilterModel treeFilter_;
    TimelineModel timeline_;
    HistoryModel history_;
    ChainEditorModel chainEditor_;
    ExampleListModel exampleList_;
    TabModel tabs_;
    int activeTabIndex_{-1};
    /// True while restoreOpenTabs re-opens saved tabs, so persistOpenTabs() doesn't rewrite
    /// settings mid-restore.
    bool restoringTabs_{false};
    KeyValueModel opHeaders_;
    KeyValueModel opQuery_;
    KeyValueModel opExtractions_;
    KeyValueModel opAssertions_;

    // Editable request surface (Edit mode), seeded on beginEdit; trailing ghost-row models grow as
    // the user types.
    EditableKeyValueModel editHeaders_;
    EditableKeyValueModel editQuery_;
    // Prevent URL-origin model resets and Params-origin path writes from
    // recursively synchronizing each other.
    int querySyncDepth_{};
    EditableKeyValueModel editForm_;
    EditableKeyValueModel editExtractions_;
    EditableKeyValueModel editAssertions_;
    EditableKeyValueModel actorConfig_;
    EditableKeyValueModel envVars_;
    QString editEnvBaseUrl_;
    EditableKeyValueModel actorAuthExtract_;
    EditableKeyValueModel actorRefreshExtract_;
    AuthStepListModel actorAuthSteps_;
    QString actorAuthMethod_;
    QString actorAuthPath_;
    QString actorAuthBody_;
    QString actorAuthExpect_;
    bool actorHasRefresh_{false};

    // Read-only actor detail selection (centre pane).
    bool hasActor_{false};
    QString selectedActorId_;
    QString selectedActorName_;
    QString selectedActorDescription_;
    QString selectedActorStrategy_;
    QString actorRefreshMethod_;
    QString actorRefreshPath_;
    QString actorRefreshBody_;
    bool actorEditing_{false};
    QString actorEditName_;
    QString actorEditDescription_;
    QString actorEditStrategy_;
    bool actorEditRefreshEnabled_{false};
    QString actorEditRefreshMethod_{QStringLiteral("POST")};
    QString actorEditRefreshPath_;
    QString actorEditRefreshBody_;
    /// The environment editor as last prepared or saved.
    EnvironmentEdits envBaseline_;
    DependencyEditModel editDependencies_;
    /// An action waiting on the unsaved-changes prompt: the unsaved tabs it would drop, and what
    /// to do once they are saved or discarded.
    struct PendingUnsaved {
        std::vector<std::uint64_t> uids;
        std::function<void()> proceed;
    };
    std::optional<PendingUnsaved> pendingUnsaved_;
    /// Set while a project transition the person already answered for runs.
    bool leaveApproved_{false};
    /// The tab that started the OAuth2 browser flow, which receives its token.
    std::uint64_t oauthTabUid_{0};
    bool dirtyUpdatePending_{false};

    bool editing_{false};
    bool creatingOperation_{false};
    QString newOperationModule_;
    QString newOperationDraftId_;
    QString newOperationName_;
    QString editMethod_;
    QString editPath_;
    QString editActor_;
    QString editExpectStatus_;
    int editTimeout_{0};
    bool editForce_{false};
    QString editBody_;
    bool editBodyIsForm_{false};
    QString editBodyType_{QStringLiteral("none")};
    QString editGraphqlQuery_;
    QString editGraphqlVariables_;
    QString editAuthType_{QStringLiteral("none")};
    QString editAuthToken_;
    QString editAuthUsername_;
    QString editAuthPassword_;
    QString editAuthApiKeyName_;
    QString editAuthApiKeyValue_;
    bool editAuthApiKeyInQuery_{false};
    QString editAuthAwsAccessKey_;
    QString editAuthAwsSecretKey_;
    QString editAuthAwsRegion_;
    QString editAuthAwsService_;
    QString editAuthAwsSessionToken_;
    QString editAuthOauthConsumerKey_;
    QString editAuthOauthConsumerSecret_;
    QString editAuthOauthToken_;
    QString editAuthOauthTokenSecret_;
    QString editAuthOauth2GrantType_{QStringLiteral("client_credentials")};
    QString editAuthOauth2ClientAuth_{QStringLiteral("basic")};
    QString editAuthOauth2AuthUrl_;
    QString editAuthOauth2CallbackUrl_{QStringLiteral("http://127.0.0.1:8080/callback")};
    QString editAuthOauth2PkceMethod_{QStringLiteral("S256")};
    QString editAuthOauth2AccessToken_;  ///< Ephemeral; never persisted to YAML.
    std::unique_ptr<OAuth2AuthCodeFlow> oauthFlow_;
    // An OAuth2 authorization waiting for the person to approve its token URL's host (FR-15.9):
    // the token URL and what starts the flow once approved.
    QString pendingOauthTokenUrl_;
    std::function<void()> pendingOauthStart_;
    QString editAuthOauth2TokenUrl_;
    QString editAuthOauth2ClientId_;
    QString editAuthOauth2ClientSecret_;
    QString editAuthOauth2Scope_;
    QString editAuthJwtAlgorithm_{QStringLiteral("HS256")};
    QString editAuthJwtSecret_;
    QString editAuthJwtPayload_;
    QString editAuthMtlsFormat_{QStringLiteral("pem")};
    QString editAuthMtlsCertPath_;
    QString editAuthMtlsKeyPath_;
    QString editAuthMtlsKeyPassword_;
    QString editAuthMtlsCaCertPath_;
    /// True once the Chain tab is seeded for the open op, so chainEdited is only stamped on a real
    /// snapshot.
    bool chainFieldsLoaded_{false};
    QString projectName_;
    QString status_;
    QString selectedModule_;
    bool hasOperation_{false};
    QString opName_;
    QString opMethod_;
    QString opPath_;
    QString opActor_;
    QString opBody_;
    QStringList opDependencies_;

    QStringList environments_;
    QString environment_;
    /// The new name while the active environment is being renamed, so the selection follows it.
    QString environmentRenameTarget_;
    /// The last chain-graph resolution, reused until any of its inputs change.
    struct ChainPlanCache {
        std::shared_ptr<const engine::Project> source;
        QString target;
        bool creating{false};
        bool patchedDeps{false};
        std::vector<std::string> deps;
        std::shared_ptr<const engine::Project> project;
        std::expected<engine::ResolvedPlan, engine::ReqloomError> plan{
            std::unexpected(engine::ReqloomError{})};
    };
    mutable std::optional<ChainPlanCache> chainPlanCache_;
    /// The chain graph's plan for the open operation, resolved at most once per edit state; null
    /// when there is nothing to resolve.
    [[nodiscard]] const ChainPlanCache* chainPlan() const;
    bool running_{false};
    RunTabRouter runRouter_;
    // Live chain-graph run state: op→status token feeds colours.
    QHash<QString, QString> chainStatus_;
    // Value-picker pins: "<resource>.<var>" → chosen concrete value.
    QHash<QString, QString> variableOverrides_;
    // Producer op whose response is saved as an example when it lands (set by refreshCandidates).
    QString pendingCandidateOp_;
    bool captureBodies_{true};
    bool hasResponse_{false};
    int respStatus_{0};
    int respElapsedMs_{0};
    int respBodySize_{0};
    QString respHeaders_;
    QString respBody_;
    ResponseBodyModel responseBody_;
    QString runOutcome_;
    QString shownExample_;
};

}  // namespace reqloom::desktop::qml
