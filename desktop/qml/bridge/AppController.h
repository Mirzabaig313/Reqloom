// AppController — the QML-facing application controller. Owns the (UI-agnostic)
// ProjectModel logic and the QML-facing ResourceListModel, and exposes project
// state to QML. C++ owns logic/state; QML binds to these properties.
#pragma once

#include "DependencyEditModel.h"
#include "EditableKeyValueModel.h"
#include "ExampleListModel.h"
#include "KeyValueModel.h"
#include "OperationListModel.h"
#include "ProjectTreeFilterModel.h"
#include "ProjectTreeModel.h"
#include "ResourceListModel.h"
#include "TimelineModel.h"

#include "../../src/application/Bootstrapper.h"
#include "../../src/application/EnvironmentSettings.h"
#include "../../src/application/RunController.h"
#include "../../src/application/SavedResponseStore.h"

#include <QtQml/qqmlregistration.h>
#include <QtCore/QAbstractItemModel>
#include <QtCore/QObject>
#include <QtCore/QSettings>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QUrl>
#include <QtCore/QVariant>

#include <memory>

namespace reqloom::desktop {
class ProjectModel;
}  // namespace reqloom::desktop

class QQmlEngine;
class QJSEngine;

namespace reqloom::desktop::qml {

class AppController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QString projectName READ projectName NOTIFY projectChanged)
    Q_PROPERTY(int resourceCount READ resourceCount NOTIFY projectChanged)
    Q_PROPERTY(QString status READ status NOTIFY projectChanged)
    Q_PROPERTY(ResourceListModel* resources READ resources CONSTANT)
    Q_PROPERTY(OperationListModel* operations READ operations CONSTANT)
    Q_PROPERTY(QString selectedModule READ selectedModule NOTIFY selectionChanged)

    // Explorer tree (Actors + Resources → operations → examples) + its filter.
    Q_PROPERTY(QAbstractItemModel* explorerModel READ explorerModel CONSTANT)
    Q_PROPERTY(int operationCount READ operationCount NOTIFY projectChanged)
    Q_PROPERTY(int actorCount READ actorCount NOTIFY projectChanged)
    // Pickers for the create dialogs.
    Q_PROPERTY(QStringList moduleNames READ moduleNames NOTIFY projectChanged)
    Q_PROPERTY(QStringList actorNames READ actorNames NOTIFY projectChanged)
    Q_PROPERTY(QStringList operationIds READ operationIds NOTIFY projectChanged)
    // Editable models backing the New Endpoint dialog's optional chain section.
    Q_PROPERTY(DependencyEditModel* newEndpointDependencies READ newEndpointDependencies CONSTANT)
    Q_PROPERTY(EditableKeyValueModel* newEndpointExtractions READ newEndpointExtractions CONSTANT)

    // Selected operation (request editor)
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
    Q_PROPERTY(QString runOutcome READ runOutcome NOTIFY responseChanged)
    /// Name of the saved example currently shown (empty when showing a live run).
    Q_PROPERTY(QString shownExample READ shownExample NOTIFY responseChanged)

    // Live run timeline + saved examples for the open operation.
    Q_PROPERTY(TimelineModel* timeline READ timeline CONSTANT)
    Q_PROPERTY(ExampleListModel* examples READ examples CONSTANT)

    // ── Editable request surface (Override / Edit mode, WS-B) ──────────────
    // `editing` toggles the read preview ↔ editable controls. The editable
    // scalars + models are seeded from the open operation on beginEdit (a
    // faithful copy the user then tweaks); applyAndRun builds a one-shot
    // RequestOverride, saveOperation patches the real op and persists it.
    Q_PROPERTY(bool editing READ editing NOTIFY editingChanged)
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
    Q_PROPERTY(EditableKeyValueModel* editHeaders READ editHeaders CONSTANT)
    Q_PROPERTY(EditableKeyValueModel* editQuery READ editQuery CONSTANT)
    Q_PROPERTY(EditableKeyValueModel* editForm READ editForm CONSTANT)
    Q_PROPERTY(EditableKeyValueModel* editExtractions READ editExtractions CONSTANT)
    Q_PROPERTY(DependencyEditModel* editDependencies READ editDependencies CONSTANT)
    Q_PROPERTY(
        QStringList editDependencyCandidates READ editDependencyCandidates NOTIFY editChanged)
    // Live per-tab count badges (Postman-style "Headers 8").
    Q_PROPERTY(int editParamsCount READ editParamsCount NOTIFY editChanged)
    Q_PROPERTY(int editHeadersCount READ editHeadersCount NOTIFY editChanged)
    Q_PROPERTY(bool editBodyFilled READ editBodyFilled NOTIFY editChanged)
    Q_PROPERTY(int editChainCount READ editChainCount NOTIFY editChanged)
    // Execution-chain nodes for the visual preview ({operationId, method,
    // isTarget}). Recomputed from the open op (read) or the edited deps (edit).
    Q_PROPERTY(QVariantList chainNodes READ chainNodes NOTIFY chainChanged)

public:
    explicit AppController(QObject* parent = nullptr);
    ~AppController() override;

    AppController(const AppController&) = delete;
    AppController& operator=(const AppController&) = delete;
    AppController(AppController&&) = delete;
    AppController& operator=(AppController&&) = delete;

    static AppController* create(QQmlEngine*, QJSEngine*) { return new AppController; }

    [[nodiscard]] QString projectName() const { return projectName_; }
    [[nodiscard]] int resourceCount() const;
    [[nodiscard]] QString status() const { return status_; }
    /// Returns a raw pointer to the current ProjectModel for C++ consumers
    /// such as SecretsController (never exposed as a QML property).
    [[nodiscard]] const ProjectModel* projectRaw() const noexcept { return project_.get(); }
    [[nodiscard]] ResourceListModel* resources() { return &resources_; }
    [[nodiscard]] OperationListModel* operations() { return &operations_; }
    [[nodiscard]] QString selectedModule() const { return selectedModule_; }

    [[nodiscard]] QAbstractItemModel* explorerModel() { return &treeFilter_; }
    [[nodiscard]] int operationCount() const;
    [[nodiscard]] int actorCount() const;
    [[nodiscard]] QStringList moduleNames() const;
    [[nodiscard]] QStringList actorNames() const;
    [[nodiscard]] QStringList operationIds() const;
    [[nodiscard]] DependencyEditModel* newEndpointDependencies() { return &newEndpointDeps_; }
    [[nodiscard]] EditableKeyValueModel* newEndpointExtractions() {
        return &newEndpointExtractions_;
    }

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
    [[nodiscard]] QString runOutcome() const { return runOutcome_; }
    [[nodiscard]] QString shownExample() const { return shownExample_; }
    [[nodiscard]] TimelineModel* timeline() { return &timeline_; }
    [[nodiscard]] ExampleListModel* examples() { return &exampleList_; }

    // ── Editable request surface accessors ────────────────────────────────
    [[nodiscard]] bool editing() const { return editing_; }
    [[nodiscard]] QString editMethod() const { return editMethod_; }
    [[nodiscard]] QString editPath() const { return editPath_; }
    [[nodiscard]] QString editActor() const { return editActor_; }
    [[nodiscard]] QString editExpectStatus() const { return editExpectStatus_; }
    [[nodiscard]] int editTimeout() const { return editTimeout_; }
    [[nodiscard]] bool editForce() const { return editForce_; }
    [[nodiscard]] QString editBody() const { return editBody_; }
    [[nodiscard]] bool editBodyIsForm() const { return editBodyIsForm_; }
    [[nodiscard]] QString editBodyType() const { return editBodyType_; }
    void setEditMethod(const QString& method);
    void setEditPath(const QString& path);
    void setEditActor(const QString& actor);
    void setEditExpectStatus(const QString& expectStatus);
    void setEditTimeout(int timeoutMs);
    void setEditForce(bool force);
    void setEditBody(const QString& body);
    void setEditBodyIsForm(bool isForm);
    /// Body kind selector: "none", "form-data", "x-www-form-urlencoded",
    /// "json", "xml", "text", or "graphql". Keeps editBodyIsForm in sync and
    /// politely sets the Content-Type header to the kind's canonical type
    /// (preserving a custom Content-Type the user set themselves).
    void setEditBodyType(const QString& type);
    [[nodiscard]] EditableKeyValueModel* editHeaders() { return &editHeaders_; }
    [[nodiscard]] EditableKeyValueModel* editQuery() { return &editQuery_; }
    [[nodiscard]] EditableKeyValueModel* editForm() { return &editForm_; }
    [[nodiscard]] EditableKeyValueModel* editExtractions() { return &editExtractions_; }
    [[nodiscard]] DependencyEditModel* editDependencies() { return &editDependencies_; }
    [[nodiscard]] QStringList editDependencyCandidates() const;
    [[nodiscard]] int editParamsCount() const;
    [[nodiscard]] int editHeadersCount() const;
    [[nodiscard]] bool editBodyFilled() const;
    [[nodiscard]] int editChainCount() const;
    [[nodiscard]] QVariantList chainNodes() const;

    /// Open a project directory (the folder containing reqloom.yaml).
    Q_INVOKABLE void openProject(const QUrl& directory);

    /// Select a module by id; populates `operations` with its endpoints.
    Q_INVOKABLE void selectModule(const QString& moduleName);

    /// Open an endpoint in the editor (populates the op* properties/models).
    Q_INVOKABLE void selectOperation(const QString& moduleName, const QString& opName);
    /// Close the editor and return to the endpoint list.
    Q_INVOKABLE void closeOperation();

    /// Run the open operation end-to-end (resolves + executes its chain).
    /// `dryRun` previews without sending; `clean` resets caches first.
    Q_INVOKABLE void runSelected(bool clean, bool dryRun);

    // ── Edit mode (one-shot override + save-to-project, WS-B) ──────────────
    /// Turn on Edit mode for the open operation: seed every editable control
    /// from the operation so the edit starts as a faithful copy.
    Q_INVOKABLE void beginEdit();
    /// Leave Edit mode without applying (read preview returns).
    Q_INVOKABLE void cancelEdit();
    /// Run the open operation with the current edits applied as a one-shot
    /// RequestOverride (the loaded project is never mutated). `clean` resets
    /// caches first; `dryRun` previews without sending.
    Q_INVOKABLE void applyAndRun(bool clean, bool dryRun);
    /// Persist the current edits to the project: patch the real operation via
    /// applyOverrideToOperation, then ProjectModel::saveOperation (which
    /// validates via the engine — a chain that forms a cycle or names an
    /// undefined op is rejected with a notify() and nothing is written).
    Q_INVOKABLE void saveOperation();

    /// Cancel the in-flight run, if any.
    Q_INVOKABLE void cancelRun();
    /// Reset the run context's session + extraction caches (refused mid-run).
    Q_INVOKABLE void resetCaches();

    /// Save the currently-displayed response as a named example for the open
    /// operation (Apidog "examples"). No-op if there's no response/operation.
    Q_INVOKABLE void saveResponse(const QString& name);

    /// Show a saved example (by name) of the currently-open operation in the
    /// response panel. Convenience for the panel's examples dropdown.
    Q_INVOKABLE void showExample(const QString& name);

    /// Body of a saved example (by name) for the currently-open operation, or
    /// an empty string if there's no such example. Used by the response panel's
    /// Diff tab to compare the live body against a saved baseline.
    Q_INVOKABLE [[nodiscard]] QString exampleBody(const QString& name) const;

    /// LCS line diff of `oldText` → `newText` for the response Diff tab. Returns
    /// rows [{sign, text}] where sign is "+" (added), "-" (removed) or " "
    /// (context), ordered for display. Reuses the tested widgets::diff::lineDiff.
    Q_INVOKABLE [[nodiscard]] QVariantList lineDiff(const QString& oldText,
                                                    const QString& newText) const;

    /// Copy `text` to the system clipboard and surface a confirmation toast
    /// (`label` names what was copied, e.g. a JSONPath). Used by the Body (Tree)
    /// click-to-copy-JSONPath affordance.
    Q_INVOKABLE void copyToClipboard(const QString& text, const QString& label);

    /// Convert a file picker URL (e.g. from a QML FileDialog) to a local
    /// filesystem path. Used by the form-data editor to fill a file field
    /// with the engine's `@<path>` upload convention.
    Q_INVOKABLE [[nodiscard]] QString localFileFromUrl(const QUrl& url) const;

    // ── Explorer selection / activation by fully-qualified id ──────────────
    /// Select an operation row ("<resource>.<op>") and open it in the editor
    /// (select-to-preview). Resolves the module + endpoint.
    Q_INVOKABLE void selectOperationById(const QString& operationId);
    /// Activate an operation row (double-click / Enter): open it and run it.
    Q_INVOKABLE void activateOperationById(const QString& operationId);

    // ── Live filter ────────────────────────────────────────────────────────
    /// Set the explorer's fuzzy filter text.
    Q_INVOKABLE void setExplorerFilter(const QString& text);

    // ── Create / rename / delete (each routes through ProjectModel, which
    //    validates via the engine before writing — cycles/undefined refs are
    //    rejected and nothing changes on disk). Emit `notify` on the outcome. ─
    /// Validate a module/endpoint name the way ProjectModel does (non-empty and
    /// free of the id-breaking '.', '/', '\\'). Single-sources the rule so the
    /// dialogs can enable/disable OK without duplicating logic in QML.
    Q_INVOKABLE [[nodiscard]] bool isValidName(const QString& name) const;

    Q_INVOKABLE void createResource(const QString& name, const QString& description);

    /// Reset the New Endpoint dialog's chain editors and dependency candidates.
    /// `preselectedResource` selects a module up-front (empty = none).
    Q_INVOKABLE void prepareNewEndpoint(const QString& preselectedResource);

    /// Create an operation from the New Endpoint dialog. Dependencies and
    /// extractions are read from `newEndpointDependencies`/`newEndpointExtractions`.
    Q_INVOKABLE void createOperation(const QString& module,
                                     const QString& name,
                                     const QString& method,
                                     const QString& path,
                                     const QString& actor);

    Q_INVOKABLE void renameOperation(const QString& operationId, const QString& newName);
    Q_INVOKABLE void deleteOperation(const QString& operationId);
    Q_INVOKABLE void renameResource(const QString& resourceId, const QString& newName);
    Q_INVOKABLE void deleteResource(const QString& resourceId);

    // ── Saved examples (explorer child rows). Selection shows the stored
    //    response in the response panel; mutation routes through the
    //    SavedResponseStore and refreshes the explorer's example rows. ────────
    Q_INVOKABLE void selectExample(const QString& operationId, const QString& exampleName);
    Q_INVOKABLE void renameExample(const QString& operationId,
                                   const QString& oldName,
                                   const QString& newName);
    Q_INVOKABLE void duplicateExample(const QString& operationId, const QString& exampleName);
    Q_INVOKABLE void deleteExample(const QString& operationId, const QString& exampleName);

signals:
    void projectChanged();
    void selectionChanged();
    void operationChanged();
    void environmentChanged();
    void runningChanged();
    void responseChanged();
    void captureBodiesChanged();
    void editingChanged();
    /// Fired on any editable-field or edit-model change (drives live tab
    /// counts, the edit banner, and re-evaluation of the editable bindings).
    void editChanged();
    /// Fired when the execution-chain preview needs to re-render (operation
    /// opened, edit toggled, or dependencies edited).
    void chainChanged();
    /// Transient feedback for create/rename/delete outcomes and errors.
    /// `isError` tints the message (full Toast UI lands in WS-D).
    void notify(QString message, bool isError);

private:
    void onLoaded();
    void onLoadFailed(const QString& code, const QString& detail);
    void loadSampleIfPresent();
    /// Refresh `exampleList_` for the open operation, and rebuild the explorer
    /// tree's example child rows from the store. Called on load + after any
    /// example mutation so the panel and explorer stay in sync.
    void refreshExamples();
    /// Refresh ONLY the open operation's example list for the response panel
    /// dropdown — does not touch the explorer tree (so selecting an operation
    /// doesn't reset/collapse the TreeView). Called on every selection.
    void refreshOpenOpExamples();
    /// Fully-qualified id of the open operation ("<module>.<op>"), or empty.
    [[nodiscard]] QString currentOperationId() const;
    /// Assemble a one-shot RequestOverride from the current edit state. Mirrors
    /// the old RequestEditorPanel::buildOverride (chainEdited tracks the guard).
    [[nodiscard]] RequestOverride buildOverride() const;

    /// Set the Content-Type header in editHeaders_ to `desired`, but only when
    /// the current Content-Type is empty or a canonical type we manage — never
    /// clobber a custom Content-Type the user typed.
    void setManagedContentType(const QString& desired);

    std::unique_ptr<ProjectModel> project_;
    std::unique_ptr<Bootstrapper> bootstrapper_;
    std::unique_ptr<RunController> runController_;
    SavedResponseStore exampleStore_;
    ResourceListModel resources_;
    OperationListModel operations_;
    ProjectTreeModel tree_;
    ProjectTreeFilterModel treeFilter_;
    DependencyEditModel newEndpointDeps_;
    EditableKeyValueModel newEndpointExtractions_;
    TimelineModel timeline_;
    ExampleListModel exampleList_;
    KeyValueModel opHeaders_;
    KeyValueModel opQuery_;
    KeyValueModel opExtractions_;

    // Editable request surface (Edit mode). Seeded from the open op on
    // beginEdit; the trailing ghost-row models grow as the user types.
    EditableKeyValueModel editHeaders_;
    EditableKeyValueModel editQuery_;
    EditableKeyValueModel editForm_;
    EditableKeyValueModel editExtractions_;
    DependencyEditModel editDependencies_;
    bool editing_{false};
    QString editMethod_;
    QString editPath_;
    QString editActor_;
    QString editExpectStatus_;
    int editTimeout_{0};
    bool editForce_{false};
    QString editBody_;
    bool editBodyIsForm_{false};
    QString editBodyType_{QStringLiteral("none")};
    /// True once the Chain tab has been seeded for the open op, so the override
    /// only stamps chainEdited when the wiring it captures is a real snapshot.
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
    bool running_{false};
    bool captureBodies_{false};
    bool hasResponse_{false};
    int respStatus_{0};
    int respElapsedMs_{0};
    int respBodySize_{0};
    QString respHeaders_;
    QString respBody_;
    QString runOutcome_;
    QString shownExample_;
};

}  // namespace reqloom::desktop::qml
