// SavedResponseStore — persists named example responses per operation so a
// user can save a run's response and reload it later (the Apidog/Postman
// "saved example" concept). Stored as JSON under the project root, keyed by
// operation id. Pure Qt (no engine dependency); unit-tested.
#pragma once

#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QStringList>

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
    /// Point the store at a project. Loads the project's saved-response file if
    /// present; an empty path detaches the store (no project loaded).
    void setProjectRoot(const QString& rootPath);

    /// Saved examples for an operation, in saved order (newest last).
    [[nodiscard]] QList<SavedResponse> list(const QString& operationId) const;

    /// Operation ids that currently have at least one saved example.
    [[nodiscard]] QStringList operationIds() const;

    /// Add (or overwrite by name) a saved example for an operation and persist.
    /// Returns false if there's no project to save into.
    bool save(const QString& operationId, const SavedResponse& response);

    /// Rename an example. Fails (false) if `newName` is empty or already used
    /// by a different example of the same operation.
    bool rename(const QString& operationId, const QString& oldName, const QString& newName);

    /// Duplicate an example under a fresh, unique name; persists. Returns the
    /// new name, or an empty string if the source wasn't found.
    QString duplicate(const QString& operationId, const QString& name);

    /// Remove a saved example by name; persists. No-op if it doesn't exist.
    void remove(const QString& operationId, const QString& name);

private:
    [[nodiscard]] QString filePath() const;
    void load();
    void persist() const;

    QString rootPath_;
    // operationId -> ordered list of saved examples.
    QList<QPair<QString, QList<SavedResponse>>> byOperation_;
};

}  // namespace reqloom::desktop
