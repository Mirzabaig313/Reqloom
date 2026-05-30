// Desktop-side facade over the engine's keychain-backed SecretStore.
// Answers "which {{secret.NAME}} does this project reference, and which are
// already present in the OS keychain?" and lets the user set / clear them.
//
// Security posture (PRD FR-11.3/11.4): values are written straight to the OS
// keychain via the engine's QtKeychain backend. This class never returns a
// stored secret's value to the UI — only presence — so a value can't leak
// into a log, a screenshot, or the clipboard by accident.
#pragma once

#include <chainapi/engine/PublicApi.h>

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <memory>

namespace chainapi::desktop {

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

    /// Test seam: inject a SecretStore (e.g. an in-memory fake) instead of
    /// the OS keychain. Production code uses the default constructor.
    SecretManager(std::unique_ptr<engine::SecretStore> store, QObject* parent);

    ~SecretManager() override;

    SecretManager(const SecretManager&) = delete;
    SecretManager& operator=(const SecretManager&) = delete;
    SecretManager(SecretManager&&) = delete;
    SecretManager& operator=(SecretManager&&) = delete;

    /// The distinct `{{secret.NAME}}` references the project declares, each
    /// annotated with whether the keychain currently holds a value. Sorted.
    [[nodiscard]] QList<SecretEntry> referencedSecrets(const ProjectModel& project) const;

    /// Store (create or overwrite) a secret. Returns true on success; on
    /// failure `error` carries a human-readable backend message.
    [[nodiscard]] bool store(const QString& name, const QString& value, QString& error);

    /// Remove a secret. No-op if already absent. Returns true on success.
    [[nodiscard]] bool clear(const QString& name, QString& error);

    /// Whether a real keychain backend is available. False when the engine
    /// was built without QtKeychain (the no-op store) — the UI warns rather
    /// than silently dropping writes.
    [[nodiscard]] bool backendAvailable() const noexcept;

private:
    std::unique_ptr<engine::SecretStore> store_;
    bool backendAvailable_{true};
};

}  // namespace chainapi::desktop
