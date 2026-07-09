// Owns the loaded engine::Project and exposes Qt-friendly accessors for the
// views. The Project value type stays engine-pure; this wrapper adds the
// QObject signalling the UI needs and nothing the engine shouldn't see.
#pragma once

#include <reqloom/engine/PublicApi.h>

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace reqloom::desktop {

/// Loaded-project state holder. Loading parses + validates via the engine;
/// failures surface through `loadFailed`. The owned Project must outlive any
/// in-flight run, so the UI blocks reloads while a run is active.
class ProjectModel : public QObject {
    Q_OBJECT

public:
    explicit ProjectModel(QObject* parent = nullptr);
    ~ProjectModel() override;

    ProjectModel(const ProjectModel&) = delete;
    ProjectModel& operator=(const ProjectModel&) = delete;
    ProjectModel(ProjectModel&&) = delete;
    ProjectModel& operator=(ProjectModel&&) = delete;

    /// Parse `<dir>/reqloom.yaml`. Emits `loaded` on success (and updates
    /// `project()`), or `loadFailed` with a stable code + detail otherwise.
    void loadFromDirectory(const QString& directory);

    /// Scaffold a brand-new, empty project: create `directory` if needed, write
    /// a minimal `reqloom.yaml` there via the engine writer, then load it so
    /// the model rebinds to the fresh project (emitting `loaded`). `name` is
    /// the project name; blank falls back to the folder's own name. Returns
    /// false (with `error` set and nothing loaded) if the folder can't be
    /// created or already contains a `reqloom.yaml` — the caller keeps its
    /// dialog open so the user's input isn't lost.
    [[nodiscard]] bool createProject(const QString& directory, const QString& name, QString& error);

    [[nodiscard]] bool hasProject() const noexcept;
    [[nodiscard]] const engine::Project& project() const noexcept;

    /// Shared, immutable handle to the current project. A consumer that runs
    /// work outliving the GUI's `isRunning()` guard (e.g. the off-thread
    /// engine run) captures this so a concurrent `loadFromDirectory` — which
    /// rebinds the model to a fresh project — can't dangle the in-flight run.
    /// Empty before the first successful load.
    [[nodiscard]] std::shared_ptr<const engine::Project> projectPtr() const noexcept;

    [[nodiscard]] QString name() const;

    /// Absolute project directory the current project was loaded from.
    /// Empty before the first successful load. Used as a stable key for
    /// per-project UI persistence (e.g. the selected environment).
    [[nodiscard]] QString rootPath() const;

    /// Environment names declared by the project, default first.
    [[nodiscard]] QStringList environmentNames() const;
    [[nodiscard]] QString defaultEnvironment() const;

    /// Resolve an operation by its "<resource>.<op>" id, or nullopt.
    [[nodiscard]] const engine::Operation* findOperation(
        const engine::OperationId& id) const noexcept;

    /// Replace operation `id` with `updated`, persist the whole project back to
    /// its directory as YAML, and rebind to the saved state. Returns true on
    /// success; on failure `error` carries a human-readable message and nothing
    /// is changed on disk. Emits `saved` on success.
    [[nodiscard]] bool saveOperation(const engine::OperationId& id,
                                     const engine::Operation& updated,
                                     QString& error);

    /// Apply many operation updates in one draft, validate once, and write
    /// once (so the whole-chain editor persists every step's depends_on /
    /// extract atomically rather than reloading per operation). `updates` maps
    /// "<resource>.<op>" id → the replacement operation. Emits `saved` on
    /// success; on failure `error` is set and nothing is written.
    [[nodiscard]] bool saveOperations(const std::map<std::string, engine::Operation>& updates,
                                      QString& error);

    /// Set (nullopt clears) the project-wide default auth used by operations
    /// whose inline auth type is `inherit`. Validates + persists; emits `saved`.
    [[nodiscard]] bool saveProjectDefaultAuth(const std::optional<engine::InlineAuth>& defaultAuth,
                                              QString& error);

    /// Rename operation `id` to `newName` (the part after the resource dot),
    /// persist the project, and rebind. Fails if the name is empty, malformed,
    /// or already used in the resource. Emits `saved` on success.
    [[nodiscard]] bool renameOperation(const engine::OperationId& id,
                                       const QString& newName,
                                       QString& error);

    /// Delete operation `id`, persist the project, and rebind. Emits `saved`
    /// on success.
    [[nodiscard]] bool deleteOperation(const engine::OperationId& id, QString& error);

    /// Rename resource `id` to `newName`, updating every operation's id and
    /// resource reference, persisting, and removing the stale `<old>.yaml`
    /// file. Fails on empty/duplicate names. Emits `saved` on success.
    [[nodiscard]] bool renameResource(const engine::ResourceId& id,
                                      const QString& newName,
                                      QString& error);

    /// Delete resource `id` (and all its operations), persist, and remove its
    /// `<id>.yaml` file. Emits `saved` on success.
    [[nodiscard]] bool deleteResource(const engine::ResourceId& id, QString& error);

    /// Create or update an actor. `originalId` empty → create a new actor;
    /// otherwise update that actor (renaming to `actor.id` if different,
    /// remapping operations that referenced it). The caller supplies the fully
    /// built actor (the bridge preserves fields the editor doesn't touch).
    /// Validates + persists; emits `saved` on success.
    [[nodiscard]] bool saveActor(const QString& originalId,
                                 const engine::Actor& actor,
                                 QString& error);

    /// Delete actor `id`, clear it from any operation that referenced it,
    /// persist (the writer prunes the stale `actors/<id>.yaml`), and rebind.
    /// Emits `saved` on success.
    [[nodiscard]] bool deleteActor(const engine::ActorId& id, QString& error);

    /// Create or update an environment. `originalName` empty → create a new
    /// environment named `name`; otherwise update it (renaming to `name` if
    /// different, moving its transport config and the project default when they
    /// point at the old name). `variables` replaces the env's variable map.
    /// Validates + persists (the writer prunes a renamed env's stale file);
    /// emits `saved` on success.
    [[nodiscard]] bool saveEnvironment(const QString& originalName,
                                       const QString& name,
                                       const std::map<std::string, std::string>& variables,
                                       QString& error);

    /// Delete environment `name` (and its transport config); if it was the
    /// project default, reassign the default to another environment (or empty).
    /// Persists + rebinds; emits `saved` on success.
    [[nodiscard]] bool deleteEnvironment(const QString& name, QString& error);
    /// Set the project-level p95 latency SLO budget (ms); 0 clears it.
    /// Persists the whole project and rebinds. Emits `saved` on success.
    [[nodiscard]] bool setLatencySloP95Ms(int ms, QString& error);

    /// Create an empty resource named `name` (optional `description`), persist,
    /// and rebind. Fails if the name is empty, contains id-breaking characters
    /// ('.', '/', '\'), or already exists. Emits `saved` on success.
    [[nodiscard]] bool createResource(const QString& name,
                                      const QString& description,
                                      QString& error);

    /// Create operation `name` under `resourceId` with the given method, path,
    /// actor, and optional chain wiring (`dependencies` / `extractions`),
    /// persist, and rebind. The draft is validated by the engine before
    /// writing, so a dependency that would create a cycle (or an undefined
    /// reference) is rejected and nothing changes on disk. On success returns
    /// the new fully-qualified operation id so the caller can open it in the
    /// editor; otherwise `error` carries a message. Emits `saved` on success.
    [[nodiscard]] std::optional<engine::OperationId> createOperation(
        const engine::ResourceId& resourceId,
        const QString& name,
        engine::HttpMethod method,
        const QString& pathTemplate,
        const engine::ActorId& actor,
        const std::vector<engine::OperationId>& dependencies,
        const std::vector<engine::Extraction>& extractions,
        QString& error);

signals:
    void loaded();
    void loadFailed(QString code, QString detail);
    void saved();

private:
    std::shared_ptr<const engine::Project> project_;
    std::filesystem::path root_;
};

}  // namespace reqloom::desktop
