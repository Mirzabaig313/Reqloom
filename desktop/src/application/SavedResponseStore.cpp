// SavedResponseStore — see header. One per-user SQLite file; every write is one transaction.
//
// Schema v1:
//   examples(project_key, operation_id, name, seq, status, headers, body, elapsed_ms, saved_at)
//   legacy_imports(project_key, source_path, imported_at)
// `project_key` is "id:<project id>" or "path:<canonical root>"; `seq` orders examples (newest
// highest).
#include "SavedResponseStore.h"

#include <sqlite3.h>

#include <QtCore/QByteArray>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileDevice>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QJsonValue>
#include <QtCore/QStandardPaths>
#include <QtCore/QtEnvironmentVariables>

#include <algorithm>
#include <utility>

namespace reqloom::desktop {

namespace {

constexpr auto kLegacyFileName = ".reqloom-responses.json";
constexpr int kSchemaVersion = 1;
// Another Reqloom instance may hold the write lock briefly; wait rather than fail at once.
constexpr int kBusyTimeoutMs = 5000;
// The legacy file holds every body of every example; anything larger is not one we wrote.
constexpr qint64 kMaxLegacyFileBytes = qint64{256} * 1024 * 1024;

constexpr auto kSchemaSql = R"sql(
CREATE TABLE IF NOT EXISTS examples (
    project_key  TEXT NOT NULL,
    operation_id TEXT NOT NULL,
    name         TEXT NOT NULL,
    seq          INTEGER NOT NULL,
    status       INTEGER NOT NULL,
    headers      TEXT NOT NULL,
    body         TEXT NOT NULL,
    elapsed_ms   INTEGER NOT NULL,
    saved_at     TEXT NOT NULL,
    PRIMARY KEY (project_key, operation_id, name)
);
CREATE INDEX IF NOT EXISTS examples_by_seq ON examples (project_key, seq);
CREATE TABLE IF NOT EXISTS legacy_imports (
    project_key TEXT PRIMARY KEY,
    source_path TEXT NOT NULL,
    imported_at TEXT NOT NULL
);
PRAGMA user_version = 1;
)sql";

class Statement {
public:
    Statement(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
            sqlite3_finalize(stmt_);
            stmt_ = nullptr;
        }
    }
    ~Statement() { sqlite3_finalize(stmt_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    Statement(Statement&&) = delete;
    Statement& operator=(Statement&&) = delete;

    [[nodiscard]] bool ok() const noexcept { return stmt_ != nullptr; }

    void bind(int index, const QString& text) {
        const QByteArray utf8 = text.toUtf8();
        sqlite3_bind_text(
            stmt_, index, utf8.constData(), static_cast<int>(utf8.size()), SQLITE_TRANSIENT);
    }
    void bind(int index, qint64 value) { sqlite3_bind_int64(stmt_, index, value); }

    [[nodiscard]] int step() { return stmt_ == nullptr ? SQLITE_ERROR : sqlite3_step(stmt_); }

    [[nodiscard]] QString text(int column) const {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): sqlite3 returns UTF-8 bytes
        const auto* data = reinterpret_cast<const char*>(sqlite3_column_text(stmt_, column));
        return QString::fromUtf8(data, sqlite3_column_bytes(stmt_, column));
    }
    [[nodiscard]] qint64 integer(int column) const { return sqlite3_column_int64(stmt_, column); }

private:
    sqlite3_stmt* stmt_{nullptr};
};

[[nodiscard]] QString errorText(sqlite3* db) {
    return db == nullptr ? QStringLiteral("out of memory") : QString::fromUtf8(sqlite3_errmsg(db));
}

[[nodiscard]] bool exec(sqlite3* db, const char* sql) {
    return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

/// Run `body` inside one transaction; any failure rolls everything back and leaves the reason in
/// `error` (read before the rollback, which would replace it).
template <typename Body>
[[nodiscard]] bool transact(sqlite3* db, QString& error, Body&& body) {
    if (!exec(db, "BEGIN IMMEDIATE")) {
        error = errorText(db);
        return false;
    }
    if (!std::forward<Body>(body)() || !exec(db, "COMMIT")) {
        error = errorText(db);
        (void)exec(db, "ROLLBACK");
        return false;
    }
    return true;
}

/// Run a statement that returns no rows; true on success.
[[nodiscard]] bool done(Statement& statement) {
    return statement.ok() && statement.step() == SQLITE_DONE;
}

[[nodiscard]] QString pathKey(const QString& rootPath) {
    const QFileInfo info(rootPath);
    const QString canonical = info.canonicalFilePath();
    return QStringLiteral("path:") +
           (canonical.isEmpty() ? QDir::cleanPath(info.absoluteFilePath()) : canonical);
}

[[nodiscard]] QString nowUtc() {
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
}

[[nodiscard]] qint64 nextSeq(sqlite3* db) {
    Statement max(db, "SELECT COALESCE(MAX(seq), 0) FROM examples");
    return max.step() == SQLITE_ROW ? max.integer(0) + 1 : 1;
}

[[nodiscard]] bool bodyTooLarge(const QString& body) {
    // UTF-8 is at most three bytes per UTF-16 unit, so short bodies skip the conversion.
    return body.size() * 3 > SavedResponseStore::kMaxBodyBytes &&
           body.toUtf8().size() > SavedResponseStore::kMaxBodyBytes;
}

[[nodiscard]] QString uniqueName(const QList<SavedResponse>& taken, const QString& base) {
    const auto used = [&taken](const QString& candidate) {
        return std::ranges::any_of(taken,
                                   [&candidate](const auto& r) { return r.name == candidate; });
    };
    QString candidate = QStringLiteral("%1 copy").arg(base);
    int n = 2;
    while (used(candidate)) {
        candidate = QStringLiteral("%1 copy %2").arg(base).arg(n++);
    }
    return candidate;
}

}  // namespace

struct SavedResponseStore::Db {
    sqlite3* handle{nullptr};

    Db() = default;
    ~Db() { sqlite3_close(handle); }
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;
    Db(Db&&) = delete;
    Db& operator=(Db&&) = delete;
};

SavedResponseStore::SavedResponseStore(QString databasePath)
    : databasePath_(databasePath.isEmpty() ? defaultDatabasePath() : std::move(databasePath)) {}

SavedResponseStore::~SavedResponseStore() = default;
SavedResponseStore::SavedResponseStore(SavedResponseStore&&) noexcept = default;
SavedResponseStore& SavedResponseStore::operator=(SavedResponseStore&&) noexcept = default;

QString SavedResponseStore::defaultDatabasePath() {
    if (const QString configDir = qEnvironmentVariable("REQLOOM_CONFIG_DIR");
        !configDir.isEmpty()) {
        return QDir(configDir).filePath(QStringLiteral("examples.db"));
    }
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty()) {
        return {};
    }
    return QDir(base).filePath(QStringLiteral("history/examples.db"));
}

void SavedResponseStore::setErrorHandler(ErrorHandler handler) {
    onError_ = std::move(handler);
}

QString SavedResponseStore::lastError() const {
    return lastError_;
}

bool SavedResponseStore::fail(const QString& message) {
    lastError_ = message;
    if (onError_) {
        onError_(message);
    }
    return false;
}

bool SavedResponseStore::ensureOpen() {
    if (db_) {
        return true;
    }
    if (databasePath_.isEmpty()) {
        return fail(QStringLiteral("Saved examples are unavailable: no per-user data folder."));
    }
    QDir().mkpath(QFileInfo(databasePath_).absolutePath());
    const bool existed = QFileInfo::exists(databasePath_);

    auto db = std::make_unique<Db>();
    const QByteArray path = QFile::encodeName(databasePath_);
    const int rc = sqlite3_open_v2(path.constData(),
                                   &db->handle,
                                   SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                                   nullptr);
    const auto unreadable = [this](const QString& reason) {
        return fail(
            QStringLiteral("The saved-examples database %1 could not be opened (%2). It was "
                           "left as it is; move it away to start a new one.")
                .arg(QDir::toNativeSeparators(databasePath_), reason));
    };
    if (rc != SQLITE_OK) {
        return unreadable(errorText(db->handle));
    }
    sqlite3_busy_timeout(db->handle, kBusyTimeoutMs);

    qint64 found{0};
    {
        Statement version(db->handle, "PRAGMA user_version");
        if (version.step() != SQLITE_ROW) {
            return unreadable(errorText(db->handle));
        }
        found = version.integer(0);
    }
    if (found > kSchemaVersion) {
        return fail(QStringLiteral("The saved-examples database %1 was made by a newer Reqloom; it "
                                   "was left as it is.")
                        .arg(QDir::toNativeSeparators(databasePath_)));
    }
    QString schemaError;
    if (found < kSchemaVersion &&
        !transact(db->handle, schemaError, [&db]() { return exec(db->handle, kSchemaSql); })) {
        return unreadable(schemaError);
    }
    if (!existed) {
        // Bodies are stored unmasked, so only the owner may read the file.
        QFile::setPermissions(databasePath_, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
    db_ = std::move(db);
    return true;
}

bool SavedResponseStore::setProjectRoot(const QString& rootPath, const QString& projectId) {
    const QString key =
        rootPath.isEmpty()
            ? QString{}
            : (projectId.isEmpty() ? pathKey(rootPath) : QStringLiteral("id:") + projectId);
    if (key == projectKey_ && rootPath == rootPath_) {
        return key.isEmpty() || db_ != nullptr;
    }
    rootPath_ = rootPath;
    projectKey_ = key;
    byOperation_.clear();
    lastError_.clear();
    if (key.isEmpty()) {
        return true;
    }
    if (!ensureOpen()) {
        return false;
    }
    if (!projectId.isEmpty()) {
        // A project gets its id after examples may already be saved under its folder.
        const QString folderKey = pathKey(rootPath);
        bool underFolder = false;
        {
            Statement any(db_->handle,
                          "SELECT 1 FROM examples WHERE project_key = ?1 UNION ALL "
                          "SELECT 1 FROM legacy_imports WHERE project_key = ?1 LIMIT 1");
            any.bind(1, folderKey);
            underFolder = any.step() == SQLITE_ROW;
        }
        if (underFolder) {
            QString error;
            const bool moved = transact(db_->handle, error, [this, &folderKey]() {
                Statement examples(db_->handle,
                                   "UPDATE OR IGNORE examples SET project_key = ?1 "
                                   "WHERE project_key = ?2");
                examples.bind(1, projectKey_);
                examples.bind(2, folderKey);
                Statement marker(db_->handle,
                                 "UPDATE OR IGNORE legacy_imports SET project_key = ?1 "
                                 "WHERE project_key = ?2");
                marker.bind(1, projectKey_);
                marker.bind(2, folderKey);
                return done(examples) && done(marker);
            });
            if (!moved) {
                (void)fail(QStringLiteral("Could not move saved examples to the project id: %1")
                               .arg(error));
            }
        }
    }
    reloadCache();
    return lastError_.isEmpty();
}

void SavedResponseStore::reloadCache() {
    byOperation_.clear();
    Statement rows(db_->handle,
                   "SELECT operation_id, name, status, headers, body, elapsed_ms FROM examples "
                   "WHERE project_key = ?1 ORDER BY seq");
    rows.bind(1, projectKey_);
    int rc = rows.step();
    while (rc == SQLITE_ROW) {
        SavedResponse r;
        const QString operationId = rows.text(0);
        r.name = rows.text(1);
        r.status = static_cast<int>(rows.integer(2));
        r.headers = rows.text(3);
        r.body = rows.text(4);
        r.elapsedMs = rows.integer(5);
        const auto it = std::ranges::find_if(
            byOperation_, [&operationId](const auto& e) { return e.first == operationId; });
        if (it == byOperation_.end()) {
            byOperation_.append({operationId, {r}});
        } else {
            it->second.append(r);
        }
        rc = rows.step();
    }
    if (rc != SQLITE_DONE) {
        byOperation_.clear();
        (void)fail(QStringLiteral("Could not read saved examples: %1").arg(errorText(db_->handle)));
    }
}

SavedResponseStore::LegacyImport SavedResponseStore::importLegacyFile() {
    LegacyImport report;
    if (projectKey_.isEmpty() || !db_) {
        return report;
    }
    report.filePath = QDir(rootPath_).filePath(QString::fromUtf8(kLegacyFileName));
    const QFileInfo info(report.filePath);
    if (!info.isFile()) {
        return report;
    }
    Statement marker(db_->handle, "SELECT 1 FROM legacy_imports WHERE project_key = ?1");
    marker.bind(1, projectKey_);
    if (marker.step() == SQLITE_ROW) {
        report.result = LegacyImport::Result::AlreadyImported;
        return report;
    }

    report.result = LegacyImport::Result::Unreadable;
    if (info.size() > kMaxLegacyFileBytes) {
        report.detail = QStringLiteral("the file is larger than 256 MiB");
        return report;
    }
    QFile file(report.filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        report.detail = file.errorString();
        return report;
    }
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        report.detail = parseError.errorString();
        return report;
    }
    const QJsonValue operations = doc.object().value(QStringLiteral("operations"));
    if (!doc.isObject() || !operations.isObject()) {
        report.detail = QStringLiteral("no \"operations\" object");
        return report;
    }

    const QJsonObject ops = operations.toObject();
    QString error;
    const bool written = transact(db_->handle, error, [&]() {
        qint64 seq = nextSeq(db_->handle);
        for (auto op = ops.constBegin(); op != ops.constEnd(); ++op) {
            for (const QJsonValue& value : op.value().toArray()) {
                const QJsonObject obj = value.toObject();
                const QString name = obj.value(QStringLiteral("name")).toString();
                const QString body = obj.value(QStringLiteral("body")).toString();
                if (name.trimmed().isEmpty() || bodyTooLarge(body)) {
                    ++report.skipped;
                    continue;
                }
                Statement insert(db_->handle,
                                 "INSERT OR IGNORE INTO examples (project_key, operation_id, name, "
                                 "seq, status, headers, body, elapsed_ms, saved_at) "
                                 "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)");
                insert.bind(1, projectKey_);
                insert.bind(2, op.key());
                insert.bind(3, name);
                insert.bind(4, seq++);
                insert.bind(5, static_cast<qint64>(obj.value(QStringLiteral("status")).toInt()));
                insert.bind(6, obj.value(QStringLiteral("headers")).toString());
                insert.bind(7, body);
                insert.bind(8,
                            static_cast<qint64>(obj.value(QStringLiteral("elapsedMs")).toDouble()));
                insert.bind(9, nowUtc());
                if (!done(insert)) {
                    return false;
                }
                report.imported += sqlite3_changes(db_->handle);
            }
        }
        Statement record(db_->handle,
                         "INSERT INTO legacy_imports (project_key, source_path, imported_at) "
                         "VALUES (?1, ?2, ?3)");
        record.bind(1, projectKey_);
        record.bind(2, report.filePath);
        record.bind(3, nowUtc());
        return done(record);
    });
    if (!written) {
        report.result = LegacyImport::Result::Failed;
        report.imported = 0;
        report.detail = error;
        (void)fail(QStringLiteral("Could not import %1: %2")
                       .arg(QDir::toNativeSeparators(report.filePath), report.detail));
        return report;
    }
    report.result = LegacyImport::Result::Imported;
    reloadCache();
    return report;
}

QList<SavedResponse> SavedResponseStore::list(const QString& operationId) const {
    for (const auto& [id, responses] : byOperation_) {
        if (id == operationId) {
            return responses;
        }
    }
    return {};
}

QStringList SavedResponseStore::operationIds() const {
    QStringList ids;
    ids.reserve(static_cast<qsizetype>(byOperation_.size()));
    for (const auto& [id, responses] : byOperation_) {
        if (!responses.isEmpty()) {
            ids.append(id);
        }
    }
    return ids;
}

bool SavedResponseStore::save(const QString& operationId, const SavedResponse& response) {
    if (projectKey_.isEmpty()) {
        return fail(QStringLiteral("No project to save the example into"));
    }
    if (bodyTooLarge(response.body)) {
        return fail(QStringLiteral("Example “%1” was not saved: its body is larger than "
                                   "%2 MiB")
                        .arg(response.name)
                        .arg(kMaxBodyBytes / (1024 * 1024)));
    }
    if (!ensureOpen()) {
        return false;
    }
    QString error;
    const bool written = transact(db_->handle, error, [&]() {
        Statement upsert(
            db_->handle,
            "INSERT INTO examples (project_key, operation_id, name, seq, status, headers, body, "
            "elapsed_ms, saved_at) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9) "
            "ON CONFLICT (project_key, operation_id, name) DO UPDATE SET status = excluded.status, "
            "headers = excluded.headers, body = excluded.body, elapsed_ms = excluded.elapsed_ms, "
            "saved_at = excluded.saved_at");
        upsert.bind(1, projectKey_);
        upsert.bind(2, operationId);
        upsert.bind(3, response.name);
        upsert.bind(4, nextSeq(db_->handle));
        upsert.bind(5, static_cast<qint64>(response.status));
        upsert.bind(6, response.headers);
        upsert.bind(7, response.body);
        upsert.bind(8, response.elapsedMs);
        upsert.bind(9, nowUtc());
        return done(upsert);
    });
    if (!written) {
        return fail(QStringLiteral("Could not save example “%1”: %2").arg(response.name, error));
    }
    lastError_.clear();
    for (auto& [id, responses] : byOperation_) {
        if (id == operationId) {
            const auto existing = std::ranges::find_if(
                responses, [&response](const auto& r) { return r.name == response.name; });
            if (existing == responses.end()) {
                responses.append(response);
            } else {
                *existing = response;
            }
            return true;
        }
    }
    byOperation_.append({operationId, {response}});
    return true;
}

bool SavedResponseStore::rename(const QString& operationId,
                                const QString& oldName,
                                const QString& newName) {
    lastError_.clear();
    if (newName.trimmed().isEmpty() || !db_) {
        return false;
    }
    for (auto& [id, responses] : byOperation_) {
        if (id != operationId) {
            continue;
        }
        if (std::ranges::any_of(
                responses, [&](const auto& r) { return r.name == newName && r.name != oldName; })) {
            return false;
        }
        const auto target = std::ranges::find_if(
            responses, [&oldName](const auto& r) { return r.name == oldName; });
        if (target == responses.end()) {
            return false;
        }
        QString error;
        const bool written = transact(db_->handle, error, [&]() {
            Statement update(db_->handle,
                             "UPDATE examples SET name = ?1 WHERE project_key = ?2 AND "
                             "operation_id = ?3 AND name = ?4");
            update.bind(1, newName);
            update.bind(2, projectKey_);
            update.bind(3, operationId);
            update.bind(4, oldName);
            return done(update);
        });
        if (!written) {
            return fail(QStringLiteral("Could not rename example “%1”: %2").arg(oldName, error));
        }
        target->name = newName;
        return true;
    }
    return false;
}

QString SavedResponseStore::duplicate(const QString& operationId, const QString& name) {
    if (!db_) {
        return {};
    }
    for (auto& [id, responses] : byOperation_) {
        if (id != operationId) {
            continue;
        }
        const auto src =
            std::ranges::find_if(responses, [&name](const auto& r) { return r.name == name; });
        if (src == responses.end()) {
            return {};
        }
        SavedResponse copy = *src;
        copy.name = uniqueName(responses, name);
        QString error;
        const bool written = transact(db_->handle, error, [&]() {
            Statement insert(db_->handle,
                             "INSERT INTO examples (project_key, operation_id, name, seq, status, "
                             "headers, body, elapsed_ms, saved_at) SELECT project_key, "
                             "operation_id, ?1, ?2, status, headers, body, elapsed_ms, ?3 FROM "
                             "examples WHERE project_key = ?4 AND operation_id = ?5 AND name = ?6");
            insert.bind(1, copy.name);
            insert.bind(2, nextSeq(db_->handle));
            insert.bind(3, nowUtc());
            insert.bind(4, projectKey_);
            insert.bind(5, operationId);
            insert.bind(6, name);
            return done(insert);
        });
        if (!written) {
            (void)fail(QStringLiteral("Could not duplicate example “%1”: %2").arg(name, error));
            return {};
        }
        responses.append(copy);
        return copy.name;
    }
    return {};
}

void SavedResponseStore::remove(const QString& operationId, const QString& name) {
    lastError_.clear();
    if (!db_) {
        return;
    }
    for (auto& [id, responses] : byOperation_) {
        if (id != operationId) {
            continue;
        }
        if (std::ranges::none_of(responses, [&name](const auto& r) { return r.name == name; })) {
            return;
        }
        QString error;
        const bool written = transact(db_->handle, error, [&]() {
            Statement erase(db_->handle,
                            "DELETE FROM examples WHERE project_key = ?1 AND operation_id = ?2 "
                            "AND name = ?3");
            erase.bind(1, projectKey_);
            erase.bind(2, operationId);
            erase.bind(3, name);
            return done(erase);
        });
        if (!written) {
            (void)fail(QStringLiteral("Could not delete example “%1”: %2").arg(name, error));
            return;
        }
        responses.removeIf([&name](const SavedResponse& r) { return r.name == name; });
        return;
    }
}

bool SavedResponseStore::renameOperation(const QString& oldId, const QString& newId) {
    const auto from =
        std::ranges::find_if(byOperation_, [&](const auto& e) { return e.first == oldId; });
    if (oldId == newId || from == byOperation_.end() || !db_) {
        return false;
    }
    QList<SavedResponse> target = list(newId);
    QList<QPair<QString, SavedResponse>> moves;
    for (const SavedResponse& r : from->second) {
        SavedResponse moved = r;
        if (std::ranges::any_of(target, [&r](const auto& t) { return t.name == r.name; })) {
            moved.name = uniqueName(target, r.name);
        }
        target.append(moved);
        moves.append({r.name, moved});
    }
    QString error;
    const bool written = transact(db_->handle, error, [&]() {
        const qint64 offset = nextSeq(db_->handle);
        for (const auto& [oldName, moved] : moves) {
            Statement update(db_->handle,
                             "UPDATE examples SET operation_id = ?1, name = ?2, seq = seq + ?3 "
                             "WHERE project_key = ?4 AND operation_id = ?5 AND name = ?6");
            update.bind(1, newId);
            update.bind(2, moved.name);
            update.bind(3, offset);
            update.bind(4, projectKey_);
            update.bind(5, oldId);
            update.bind(6, oldName);
            if (!done(update)) {
                return false;
            }
        }
        return true;
    });
    if (!written) {
        return fail(
            QStringLiteral("Could not move the examples of %1 to %2: %3").arg(oldId, newId, error));
    }
    byOperation_.erase(from);
    const auto to =
        std::ranges::find_if(byOperation_, [&](const auto& e) { return e.first == newId; });
    if (to == byOperation_.end()) {
        byOperation_.append({newId, target});
    } else {
        to->second = target;
    }
    return true;
}

bool SavedResponseStore::clearOperation(const QString& operationId) {
    if (projectKey_.isEmpty() || !ensureOpen()) {
        return false;
    }
    QString error;
    const bool written = transact(db_->handle, error, [&]() {
        Statement erase(db_->handle,
                        "DELETE FROM examples WHERE project_key = ?1 AND operation_id = ?2");
        erase.bind(1, projectKey_);
        erase.bind(2, operationId);
        return done(erase);
    });
    if (!written) {
        return fail(
            QStringLiteral("Could not clear the examples of %1: %2").arg(operationId, error));
    }
    byOperation_.removeIf([&operationId](const auto& e) { return e.first == operationId; });
    return true;
}

bool SavedResponseStore::clearProject() {
    if (projectKey_.isEmpty() || !ensureOpen()) {
        return false;
    }
    QString error;
    const bool written = transact(db_->handle, error, [&]() {
        Statement erase(db_->handle, "DELETE FROM examples WHERE project_key = ?1");
        erase.bind(1, projectKey_);
        return done(erase);
    });
    if (!written) {
        return fail(QStringLiteral("Could not clear saved examples: %1").arg(error));
    }
    byOperation_.clear();
    return true;
}

}  // namespace reqloom::desktop
