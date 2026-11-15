// Desktop-side facade over the engine's keychain-backed SecretStore.
// Answers "which {{secret.NAME}} does this project reference, and which are
// already present in the OS keychain?" and lets the user set / clear them.
//
// Every read or write first checks the project's folder against the per-user trust record,
// so a project copying another's id cannot touch its secrets.
//
// Security posture (PRD FR-11.3/11.4): values are written straight to the OS
// keychain via the engine's QtKeychain backend. This class never returns a
// stored secret's value to the UI — only presence — so a value can't leak
// into a log, a screenshot, or the clipboard by accident.
#pragma once

#include <reqloom/engine/ProjectTrustStore.h>
#include <reqloom/engine/PublicApi.h>

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <memory>

namespace reqloom::desktop {

class ProjectModel;

/// Presence state of one referenced secret in the keychain.
enum class SecretState : std::uint8_t {
    Set,        ///< a value is stored
    NotSet,     ///< referenced by the project but absent from the keychain
    ReadError,  ///< the keychain backend errored while checking
};

struct SecretEntry {
    QString name;
    SecretState state{SecretState::NotSet};
    QString detail;  ///< populated on ReadError
};

class SecretManager : public QObject {
    Q_OBJECT

public:
    explicit SecretManager(QObject* parent = nullptr);

    /// Test seam: inject a SecretStore and ProjectTrustStore (e.g. in-memory fakes).
    /// Production code uses the default constructor.
    SecretManager(std::unique_ptr<engine::SecretStore> store,
                  std::unique_ptr<engine::ProjectTrustStore> trust,
                  QObject* parent);

    ~SecretManager() override;

    SecretManager(const SecretManager&) = delete;
    SecretManager& operator=(const SecretManager&) = delete;
    SecretManager(SecretManager&&) = delete;
    SecretManager& operator=(SecretManager&&) = delete;

    /// The distinct `{{secret.NAME}}` references the project declares, each
    /// annotated with whether the keychain currently holds a value. Sorted.
    [[nodiscard]] QList<SecretEntry> referencedSecrets(const ProjectModel& project) const;

    /// Store (create or overwrite) one of `project`'s secrets, namespaced by project id.
    /// On failure `error` carries a human-readable message.
    [[nodiscard]] bool store(const ProjectModel& project,
                             const QString& name,
                             const QString& value,
                             QString& error);

    /// Remove one of `project`'s secrets; no-op if already absent.
    [[nodiscard]] bool clear(const ProjectModel& project, const QString& name, QString& error);

    /// The folder this machine records for the project's id when this folder now claims it
    /// (moved or copied project); empty when none. While non-empty, all reads and writes are
    /// refused.
    [[nodiscard]] QString folderConflict(const ProjectModel& project) const;

    /// Bind the project's id to its current folder after the user confirms it was moved.
    /// `error` carries the reason on failure.
    [[nodiscard]] bool confirmMove(const ProjectModel& project, QString& error);

    /// Whether a real keychain backend is available. False when the engine
    /// was built without QtKeychain (the no-op store) — the UI warns rather
    /// than silently dropping writes.
    [[nodiscard]] bool backendAvailable() const noexcept;

private:
    std::unique_ptr<engine::SecretStore> store_;
    std::unique_ptr<engine::ProjectTrustStore> trust_;
    bool backendAvailable_{true};
};

}  // namespace reqloom::desktop
