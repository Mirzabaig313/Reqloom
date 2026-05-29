// Owns the loaded engine::Project and exposes Qt-friendly accessors for the
// views. The Project value type stays engine-pure; this wrapper adds the
// QObject signalling the UI needs and nothing the engine shouldn't see.
#pragma once

#include <chainapi/engine/PublicApi.h>

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <filesystem>
#include <optional>

namespace chainapi::desktop {

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

    /// Parse `<dir>/chainapi.yaml`. Emits `loaded` on success (and updates
    /// `project()`), or `loadFailed` with a stable code + detail otherwise.
    void loadFromDirectory(const QString& directory);

    [[nodiscard]] bool hasProject() const noexcept;
    [[nodiscard]] const engine::Project& project() const noexcept;
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

signals:
    void loaded();
    void loadFailed(QString code, QString detail);

private:
    std::optional<engine::Project> project_;
    std::filesystem::path root_;
};

}  // namespace chainapi::desktop
