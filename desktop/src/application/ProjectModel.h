// Owns the loaded engine::Project and exposes Qt-friendly accessors; the engine-pure Project stays
// free of QObject signalling.
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

/// Loaded-project state holder; load failures surface through `loadFailed`. The Project must
/// outlive any in-flight run, so the UI blocks reloads while a run is active.
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

    /// Scaffold an empty project: write a minimal `reqloom.yaml` in `directory` (created if
    /// needed), then load it, emitting `loaded`. Blank `name` falls back to the folder name.
    /// Returns false (`error` set, nothing loaded) if the folder can't be created or already holds
    /// a `reqloom.yaml`.
    [[nodiscard]] bool createProject(const QString& directory, const QString& name, QString& error);

    [[nodiscard]] bool hasProject() const noexcept;
    [[nodiscard]] const engine::Project& project() const noexcept;

    /// Shared, immutable handle to the current project. Off-thread consumers capture it so a
    /// concurrent `loadFromDirectory` can't dangle an in-flight run. Empty before the first load.
    [[nodiscard]] std::shared_ptr<const engine::Project> projectPtr() const noexcept;

    [[nodiscard]] QString name() const;

    /// Absolute project directory, empty before the first load; the stable key for per-project UI
    /// persistence.
    [[nodiscard]] QString rootPath() const;

    /// Environment names declared by the project, default first.
    [[nodiscard]] QStringList environmentNames() const;
    [[nodiscard]] QString defaultEnvironment() const;

    /// Resolve an operation by its "<resource>.<op>" id, or nullopt.
    [[nodiscard]] const engine::Operation* findOperation(
        const engine::OperationId& id) const noexcept;

    /// Replace operation `id` with `updated`, persist the project as YAML and rebind. Returns false
    /// with `error` set and disk unchanged on failure. Emits `saved` on success.
    [[nodiscard]] bool saveOperation(const engine::OperationId& id,
                                     const engine::Operation& updated,
                                     QString& error);

    /// Apply many operation updates in one draft: validate once, write once (so the chain editor
    /// persists atomically). `updates` maps "<resource>.<op>" → replacement. Emits `saved`; on
    /// failure sets `error` and writes nothing.
    [[nodiscard]] bool saveOperations(const std::map<std::string, engine::Operation>& updates,
                                      QString& error);

    /// Set (nullopt clears) the project-wide default auth used by operations
    /// whose inline auth type is `inherit`. Validates + persists; emits `saved`.
    [[nodiscard]] bool saveProjectDefaultAuth(const std::optional<engine::InlineAuth>& defaultAuth,
                                              QString& error);

    /// Rename operation `id` to `newName` (the part after the resource dot). Fails if empty,
    /// malformed or already used. Emits `saved` on success.
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

    /// Create or update an actor (`originalId` empty → create; else update, renaming to `actor.id`
    /// and remapping references). The caller supplies the full actor. Emits `saved` on success.
    [[nodiscard]] bool saveActor(const QString& originalId,
                                 const engine::Actor& actor,
                                 QString& error);

    /// Delete actor `id`, clear it from operations that referenced it, persist and rebind. Emits
    /// `saved` on success.
    [[nodiscard]] bool deleteActor(const engine::ActorId& id, QString& error);

    /// Create or update an environment (`originalName` empty → create; else update, renaming to
    /// `name` and moving its transport config and default). `variables` replaces the variable map.
    /// Emits `saved` on success.
    [[nodiscard]] bool saveEnvironment(const QString& originalName,
                                       const QString& name,
                                       const std::map<std::string, std::string>& variables,
                                       QString& error);

    /// Delete environment `name` (and its transport config); if it was the
    /// project default, reassign the default to another environment (or empty).
    /// Persists + rebinds; emits `saved` on success.
    [[nodiscard]] bool deleteEnvironment(const QString& name, QString& error);
    /// Set the project-level p95 latency SLO budget (ms); 0 clears it. Emits `saved` on success.
    [[nodiscard]] bool setLatencySloP95Ms(int ms, QString& error);

    /// Create an empty resource. Fails if the name is empty, contains '.', '/' or a backslash, or
    /// already exists. Emits `saved`.
    [[nodiscard]] bool createResource(const QString& name,
                                      const QString& description,
                                      QString& error);

    /// Create `operation` under `resourceId` with `name` as its local id; its `id` and `resource`
    /// are set from the destination. The draft is validated before writing, so failure leaves
    /// memory and disk unchanged.
    /// @return The fully-qualified id, or `std::nullopt` with a message in `error`.
    [[nodiscard]] std::optional<engine::OperationId> createOperation(
        const engine::ResourceId& resourceId,
        const QString& name,
        const engine::Operation& operation,
        QString& error);

    /// Convenience overload creating an operation from basic request and chain fields. Emits
    /// `saved` on success.
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
