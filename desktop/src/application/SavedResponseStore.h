// SavedResponseStore — named example responses per operation, kept in a per-user SQLite database
// outside every project folder so they cannot be committed with the project. Pure Qt + sqlite3.
#pragma once

#include <QtCore/QList>
#include <QtCore/QPair>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <cstdint>
#include <functional>
#include <memory>

namespace reqloom::desktop {

/// One saved example response for an operation.
struct SavedResponse {
    QString name;
    int status{0};
    QString headers;
    QString body;
    qint64 elapsedMs{0};

    [[nodiscard]] bool isValid() const noexcept { return status > 0; }
};

class SavedResponseStore {
public:
    /// Largest body one example may hold, in UTF-8 bytes: the engine's capture cap
    /// (`engine::kMaxCapturedBodyBytes`), so every captured response fits.
    static constexpr qint64 kMaxBodyBytes = qint64{5} * 1024 * 1024;

    using ErrorHandler = std::function<void(const QString&)>;

    /// What `importLegacyFile` did with `<project>/.reqloom-responses.json`.
    struct LegacyImport {
        enum class Result : std::uint8_t {
            None,             ///< no file, or no project / database
            AlreadyImported,  ///< imported earlier; the file is ignored
            Imported,         ///< copied into the database just now
            Unreadable,  ///< not valid JSON (e.g. a merge conflict); skipped, retried next time
            Failed,      ///< the database write failed; `detail` says why
        };
        Result result{Result::None};
        int imported{0};
        int skipped{0};  ///< entries without a name, or with a body over `kMaxBodyBytes`
        QString filePath;
        QString detail;
    };

    /// `databasePath` empty means `defaultDatabasePath()`.
    explicit SavedResponseStore(QString databasePath = {});
    ~SavedResponseStore();
    SavedResponseStore(const SavedResponseStore&) = delete;
    SavedResponseStore& operator=(const SavedResponseStore&) = delete;
    SavedResponseStore(SavedResponseStore&&) noexcept;
    SavedResponseStore& operator=(SavedResponseStore&&) noexcept;

    /// `<REQLOOM_CONFIG_DIR>/examples.db` when that variable is set, else `examples.db` in the
    /// per-user `history` folder that also holds the run-history databases. Empty when neither
    /// location is known.
    [[nodiscard]] static QString defaultDatabasePath();

    /// Called with a user-facing message whenever opening the database or a write fails.
    void setErrorHandler(ErrorHandler handler);

    /// Point the store at a project, keyed by `projectId` when given, else by the canonical
    /// `rootPath`. Examples saved under the folder before the project had an id move to the id.
    /// An empty path detaches the store. A no-op when the project key is unchanged.
    ///
    /// @return false (and the error handler called) when the database cannot be opened; a corrupt
    ///         database is never modified or replaced.
    bool setProjectRoot(const QString& rootPath, const QString& projectId = {});

    /// Copy `<project>/.reqloom-responses.json` into the database once per project. The file is
    /// only read, never changed or deleted.
    LegacyImport importLegacyFile();

    /// Saved examples for an operation, in saved order (newest last).
    [[nodiscard]] QList<SavedResponse> list(const QString& operationId) const;

    /// Operation ids that currently have at least one saved example.
    [[nodiscard]] QStringList operationIds() const;

    /// Add (or overwrite by name) a saved example for an operation, in one transaction. Returns
    /// false (error handler called) with no project, a body over `kMaxBodyBytes`, or a failed
    /// write.
    bool save(const QString& operationId, const SavedResponse& response);

    /// Rename an example. Fails (false) if `newName` is empty or already used
    /// by a different example of the same operation, or the write fails.
    bool rename(const QString& operationId, const QString& oldName, const QString& newName);

    /// Duplicate an example under a fresh, unique name. Returns the new name, or an empty string if
    /// the source wasn't found or the write failed.
    QString duplicate(const QString& operationId, const QString& name);

    /// Remove a saved example by name. No-op if it doesn't exist.
    void remove(const QString& operationId, const QString& name);

    /// Move an operation's examples to `newId` after the operation was renamed, after any already
    /// under `newId` (a clashing name gets a " copy" suffix). Returns false when `oldId` has none.
    bool renameOperation(const QString& oldId, const QString& newId);

    /// Delete every example of one operation of the current project.
    bool clearOperation(const QString& operationId);

    /// Delete every example of the current project.
    bool clearProject();

    /// The last failure message, empty after a success.
    [[nodiscard]] QString lastError() const;

private:
    struct Db;

    [[nodiscard]] bool fail(const QString& message);
    [[nodiscard]] bool ensureOpen();
    void reloadCache();

    QString databasePath_;
    QString rootPath_;
    QString projectKey_;
    QString lastError_;
    ErrorHandler onError_;
    std::unique_ptr<Db> db_;
    // operationId -> ordered list of saved examples, mirroring the database for this project.
    QList<QPair<QString, QList<SavedResponse>>> byOperation_;
};

}  // namespace reqloom::desktop
