// SqliteHistoryStore — SQLite-backed run-history persistence.
//
// Schema v1:
//   runs(run_id, target_op, env_name, started_at, ended_at, outcome, chain_size)
//   run_events(id, run_id, seq, event_type, step_index, op_id, payload, at)
//
// Events are stored as JSON in `payload`. The `runs` table denormalises
// enough to render the history sidebar without parsing payloads; the
// JSON keeps the full variant for replay, so schema upgrades stay
// payload-format changes rather than ALTER TABLEs.
//
// WAL mode + 5s busy timeout: the engine appends from the run thread
// while the desktop reads on a separate connection without blocking.
// Instances are not thread-safe — one store per ExecutionEngine, caller
// serialises (AGENTS.md: engine entry points are single-threaded per run).

#include "SqliteHistoryStore.h"

#include "../../domain/Codecs.h"

#include <chainapi/engine/ErrorCodes.h>
#include <chainapi/engine/Events.h>

#include <sqlite3.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace chainapi::engine {

namespace {

using nlohmann::json;
namespace fs = std::filesystem;

// Compile guard for the visitor below — adding a RunEvent variant
// without a serialization arm fails the build instead of writing an
// empty event_type.
template <typename>
inline constexpr bool kAlwaysFalse = false;

constexpr int kBusyTimeoutMs = 5000;

[[nodiscard]] std::string isoUtc(TimePoint tp) {
    // Second precision is plenty for the timeline.
    using namespace std::chrono;
    const auto t = system_clock::to_time_t(tp);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string{buf};
}

[[nodiscard]] TimePoint parseIsoUtc(std::string_view s) {
    std::tm tm{};
    std::stringstream ss{std::string{s}};
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    if (ss.fail()) return {};
#if defined(_WIN32)
    const auto t = _mkgmtime(&tm);
#else
    const auto t = timegm(&tm);
#endif
    return std::chrono::system_clock::from_time_t(t);
}

[[nodiscard]] std::string_view methodToWire(HttpMethod m) {
    return codecs::methodToString(m);
}

[[nodiscard]] HttpMethod methodFromWire(std::string_view s) noexcept {
    if (s == "POST") return HttpMethod::Post;
    if (s == "PUT") return HttpMethod::Put;
    if (s == "PATCH") return HttpMethod::Patch;
    if (s == "DELETE") return HttpMethod::Delete;
    if (s == "HEAD") return HttpMethod::Head;
    if (s == "OPTIONS") return HttpMethod::Options;
    return HttpMethod::Get;
}

[[nodiscard]] std::string_view skipReasonToWire(SkipReason r) noexcept {
    return r == SkipReason::SessionValid ? "SessionValid" : "ExtractionCached";
}

[[nodiscard]] SkipReason skipReasonFromWire(std::string_view s) noexcept {
    return s == "SessionValid" ? SkipReason::SessionValid : SkipReason::ExtractionCached;
}

[[nodiscard]] std::string outcomeToWire(RunOutcome o) {
    switch (o) {
        case RunOutcome::Succeeded:
            return "Succeeded";
        case RunOutcome::Failed:
            return "Failed";
        case RunOutcome::Cancelled:
            return "Cancelled";
    }
    return "Failed";
}

[[nodiscard]] RunOutcome outcomeFromWire(std::string_view s) noexcept {
    if (s == "Succeeded") return RunOutcome::Succeeded;
    if (s == "Cancelled") return RunOutcome::Cancelled;
    return RunOutcome::Failed;
}

// ─── Header (de)serialization helpers ────────────────────────────────────────

[[nodiscard]] json headersToJson(const std::vector<std::pair<std::string, std::string>>& hs) {
    json out = json::array();
    for (const auto& [k, v] : hs) {
        out.push_back({{"k", k}, {"v", v}});
    }
    return out;
}

[[nodiscard]] std::vector<std::pair<std::string, std::string>> headersFromJson(const json& j) {
    std::vector<std::pair<std::string, std::string>> out;
    if (!j.is_array()) return out;
    for (const auto& item : j) {
        out.emplace_back(item.value("k", std::string{}), item.value("v", std::string{}));
    }
    return out;
}

// ─── RAII wrappers ───────────────────────────────────────────────────────────

struct StmtDeleter {
    void operator()(sqlite3_stmt* s) const noexcept {
        if (s != nullptr) sqlite3_finalize(s);
    }
};
using StmtPtr = std::unique_ptr<sqlite3_stmt, StmtDeleter>;

struct DbDeleter {
    void operator()(sqlite3* db) const noexcept {
        if (db != nullptr) sqlite3_close(db);
    }
};
using DbPtr = std::unique_ptr<sqlite3, DbDeleter>;

// Scopes a write to a single transaction. Begins IMMEDIATE on construction;
// rolls back on destruction unless commit() succeeded. IMMEDIATE acquires
// the write lock up front so a busy database fails fast at begin() rather
// than mid-statement. All sqlite3_exec calls here are on trusted, fixed SQL.
class SqliteTransaction {
public:
    explicit SqliteTransaction(sqlite3* db) noexcept : db_(db) {
        active_ = sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) == SQLITE_OK;
    }
    ~SqliteTransaction() {
        if (active_) {
            sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        }
    }
    SqliteTransaction(const SqliteTransaction&) = delete;
    SqliteTransaction& operator=(const SqliteTransaction&) = delete;
    SqliteTransaction(SqliteTransaction&&) = delete;
    SqliteTransaction& operator=(SqliteTransaction&&) = delete;

    [[nodiscard]] bool began() const noexcept { return active_; }

    [[nodiscard]] bool commit() noexcept {
        if (!active_) {
            return false;
        }
        const bool ok = sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) == SQLITE_OK;
        active_ = false;  // committed (or failed) — destructor must not roll back
        return ok;
    }

private:
    sqlite3* db_;
    bool active_{false};
};

[[nodiscard]] ChainApiError sqliteError(sqlite3* db, std::string_view what) {
    const char* msg = (db != nullptr) ? sqlite3_errmsg(db) : "no database";
    return ChainApiError{ErrorCode::SchemaInvalid,
                         ErrorClass::Schema,
                         std::string{what} + ": " + (msg != nullptr ? msg : "?")};
}

// ─── Event → JSON (per variant) ──────────────────────────────────────────────
//
// runId / stepIndex / event_type / op_id / at go in their own columns so
// the index can find rows without parsing JSON; the rest goes in `payload`.

struct EventEnvelope {
    std::uint64_t runId{0};
    std::optional<std::size_t> stepIndex;
    std::optional<std::string> opId;
    std::string at;
    std::string eventType;
    json payload;
};

[[nodiscard]] EventEnvelope envelopeOf(const RunEvent& ev) {
    EventEnvelope e;
    std::visit(
        [&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            e.runId = v.runId.value;
            e.at = isoUtc(v.at);

            if constexpr (std::is_same_v<T, RunStarted>) {
                e.eventType = "RunStarted";
                e.opId = v.target.value;
                e.payload = {
                    {"target", v.target.value}, {"chainSize", v.chainSize}, {"envName", v.envName}};
            } else if constexpr (std::is_same_v<T, StepStarted>) {
                e.eventType = "StepStarted";
                e.stepIndex = v.stepIndex;
                e.opId = v.op.value;
                e.payload = {{"op", v.op.value}, {"attempt", v.attempt}};
            } else if constexpr (std::is_same_v<T, StepSkipped>) {
                e.eventType = "StepSkipped";
                e.stepIndex = v.stepIndex;
                e.opId = v.op.value;
                e.payload = {{"op", v.op.value},
                             {"reason", std::string{skipReasonToWire(v.reason)}}};
            } else if constexpr (std::is_same_v<T, RequestPrepared>) {
                e.eventType = "RequestPrepared";
                e.stepIndex = v.stepIndex;
                e.payload = {{"method", std::string{methodToWire(v.method)}},
                             {"url", v.url},
                             {"headers", headersToJson(v.maskedHeaders)},
                             {"bodySize", v.bodySize}};
            } else if constexpr (std::is_same_v<T, ResponseReceived>) {
                e.eventType = "ResponseReceived";
                e.stepIndex = v.stepIndex;
                e.payload = {{"status", v.status},
                             {"headers", headersToJson(v.headers)},
                             {"bodySize", v.bodySize},
                             {"elapsedMs", v.elapsed.count()}};
            } else if constexpr (std::is_same_v<T, ExtractionApplied>) {
                e.eventType = "ExtractionApplied";
                e.stepIndex = v.stepIndex;
                e.payload = {{"resource", v.resource.value}, {"variableNames", v.variableNames}};
            } else if constexpr (std::is_same_v<T, ExtractionCompleted>) {
                e.eventType = "ExtractionCompleted";
                e.stepIndex = v.stepIndex;
                e.opId = v.op.value;
                std::string outcome;
                switch (v.outcome) {
                    case ExtractionCompleted::Outcome::Resolved:
                        outcome = "Resolved";
                        break;
                    case ExtractionCompleted::Outcome::Null:
                        outcome = "Null";
                        break;
                    case ExtractionCompleted::Outcome::Missing:
                        outcome = "Missing";
                        break;
                    case ExtractionCompleted::Outcome::InvalidPattern:
                        outcome = "InvalidPattern";
                        break;
                    case ExtractionCompleted::Outcome::Unsupported:
                        outcome = "Unsupported";
                        break;
                }
                e.payload = {{"op", v.op.value},
                             {"variableName", v.variableName},
                             {"sourcePath", v.sourcePath},
                             {"outcome", outcome},
                             {"value", v.value}};
            } else if constexpr (std::is_same_v<T, StepFailed>) {
                e.eventType = "StepFailed";
                e.stepIndex = v.stepIndex;
                e.opId = v.op.value;
                e.payload = {{"op", v.op.value},
                             {"code", std::string{toCodeString(v.code)}},
                             {"attempt", v.attempt},
                             {"detail", v.detail}};
            } else if constexpr (std::is_same_v<T, StepCancelled>) {
                e.eventType = "StepCancelled";
                e.stepIndex = v.stepIndex;
                e.opId = v.op.value;
                e.payload = {{"op", v.op.value}};
            } else if constexpr (std::is_same_v<T, SessionRefreshed>) {
                e.eventType = "SessionRefreshed";
                e.opId = v.actor.value;
                std::string trigger =
                    (v.trigger == SessionRefreshed::Trigger::Expiry) ? "Expiry" : "Unauthorized";
                e.payload = {{"actor", v.actor.value}, {"trigger", trigger}};
            } else if constexpr (std::is_same_v<T, RunEnded>) {
                e.eventType = "RunEnded";
                e.payload = {{"outcome", outcomeToWire(v.outcome)},
                             {"elapsedMs", v.elapsed.count()}};
            } else {
                static_assert(kAlwaysFalse<T>, "unhandled RunEvent variant in envelopeOf");
            }
        },
        ev);
    return e;
}

// ─── JSON → Event (per variant) ──────────────────────────────────────────────

[[nodiscard]] std::optional<RunEvent> eventFromRow(const std::string& eventType,
                                                   std::uint64_t runIdRaw,
                                                   std::optional<std::size_t> stepIndex,
                                                   const std::string& atIso,
                                                   const json& p) {
    const RunId runId{runIdRaw};
    const auto at = parseIsoUtc(atIso);

    if (eventType == "RunStarted") {
        RunStarted ev;
        ev.runId = runId;
        ev.target = OperationId{p.value("target", std::string{})};
        ev.chainSize = p.value("chainSize", std::size_t{0});
        ev.envName = p.value("envName", std::string{});
        ev.at = at;
        return ev;
    }
    if (eventType == "StepStarted") {
        StepStarted ev;
        ev.runId = runId;
        ev.stepIndex = stepIndex.value_or(0);
        ev.op = OperationId{p.value("op", std::string{})};
        ev.attempt = p.value("attempt", 1);
        ev.at = at;
        return ev;
    }
    if (eventType == "StepSkipped") {
        StepSkipped ev;
        ev.runId = runId;
        ev.stepIndex = stepIndex.value_or(0);
        ev.op = OperationId{p.value("op", std::string{})};
        ev.reason = skipReasonFromWire(p.value("reason", std::string{}));
        ev.at = at;
        return ev;
    }
    if (eventType == "RequestPrepared") {
        RequestPrepared ev;
        ev.runId = runId;
        ev.stepIndex = stepIndex.value_or(0);
        ev.method = methodFromWire(p.value("method", std::string{"GET"}));
        ev.url = p.value("url", std::string{});
        ev.maskedHeaders = headersFromJson(p.value("headers", json::array()));
        ev.bodySize = p.value("bodySize", std::size_t{0});
        ev.at = at;
        return ev;
    }
    if (eventType == "ResponseReceived") {
        ResponseReceived ev;
        ev.runId = runId;
        ev.stepIndex = stepIndex.value_or(0);
        ev.status = p.value("status", 0);
        ev.headers = headersFromJson(p.value("headers", json::array()));
        ev.bodySize = p.value("bodySize", std::size_t{0});
        ev.elapsed = std::chrono::milliseconds{p.value("elapsedMs", std::int64_t{0})};
        ev.at = at;
        return ev;
    }
    if (eventType == "ExtractionApplied") {
        ExtractionApplied ev;
        ev.runId = runId;
        ev.stepIndex = stepIndex.value_or(0);
        ev.resource = ResourceId{p.value("resource", std::string{})};
        if (p.contains("variableNames") && p["variableNames"].is_array()) {
            for (const auto& n : p["variableNames"]) {
                ev.variableNames.push_back(n.get<std::string>());
            }
        }
        ev.at = at;
        return ev;
    }
    if (eventType == "ExtractionCompleted") {
        ExtractionCompleted ev;
        ev.runId = runId;
        ev.stepIndex = stepIndex.value_or(0);
        ev.op = OperationId{p.value("op", std::string{})};
        ev.variableName = p.value("variableName", std::string{});
        ev.sourcePath = p.value("sourcePath", std::string{});
        const auto o = p.value("outcome", std::string{"Missing"});
        if (o == "Resolved")
            ev.outcome = ExtractionCompleted::Outcome::Resolved;
        else if (o == "Null")
            ev.outcome = ExtractionCompleted::Outcome::Null;
        else if (o == "InvalidPattern")
            ev.outcome = ExtractionCompleted::Outcome::InvalidPattern;
        else if (o == "Unsupported")
            ev.outcome = ExtractionCompleted::Outcome::Unsupported;
        else
            ev.outcome = ExtractionCompleted::Outcome::Missing;
        ev.value = p.value("value", std::string{});
        ev.at = at;
        return ev;
    }
    if (eventType == "StepFailed") {
        // Unknown codes (written by a newer build) fall back to
        // SchemaInvalid so the row still shows up.
        StepFailed ev;
        ev.runId = runId;
        ev.stepIndex = stepIndex.value_or(0);
        ev.op = OperationId{p.value("op", std::string{})};
        ev.attempt = p.value("attempt", 1);
        ev.detail = p.value("detail", std::string{});
        const auto codeStr = p.value("code", std::string{"E_SCHEMA_INVALID"});
        ev.code = fromCodeString(codeStr).value_or(ErrorCode::SchemaInvalid);
        ev.cls = classify(ev.code);
        ev.at = at;
        return ev;
    }
    if (eventType == "StepCancelled") {
        StepCancelled ev;
        ev.runId = runId;
        ev.stepIndex = stepIndex.value_or(0);
        ev.op = OperationId{p.value("op", std::string{})};
        ev.at = at;
        return ev;
    }
    if (eventType == "SessionRefreshed") {
        SessionRefreshed ev;
        ev.runId = runId;
        ev.actor = ActorId{p.value("actor", std::string{})};
        ev.trigger = (p.value("trigger", std::string{}) == "Expiry")
                         ? SessionRefreshed::Trigger::Expiry
                         : SessionRefreshed::Trigger::Unauthorized;
        ev.at = at;
        return ev;
    }
    if (eventType == "RunEnded") {
        RunEnded ev;
        ev.runId = runId;
        ev.outcome = outcomeFromWire(p.value("outcome", std::string{"Failed"}));
        ev.elapsed = std::chrono::milliseconds{p.value("elapsedMs", std::int64_t{0})};
        ev.at = at;
        return ev;
    }
    return std::nullopt;
}

}  // namespace

// ─── Impl ────────────────────────────────────────────────────────────────────

struct SqliteHistoryStore::Impl {
    DbPtr db;

    // Prepared once on open, reused per append — skips SQLite's per-call
    // statement-cache lookup.
    StmtPtr insertEventStmt;
    StmtPtr insertRunStmt;
    StmtPtr updateRunStartedStmt;
    StmtPtr updateRunEndedStmt;
    StmtPtr nextSeqStmt;

    [[nodiscard]] std::expected<void, ChainApiError> exec(std::string_view sql) {
        char* err = nullptr;
        const int rc = sqlite3_exec(db.get(), std::string{sql}.c_str(), nullptr, nullptr, &err);
        if (rc != SQLITE_OK) {
            ChainApiError e{ErrorCode::SchemaInvalid,
                            ErrorClass::Schema,
                            std::string{"history: exec failed: "} + (err != nullptr ? err : "?")};
            sqlite3_free(err);
            return std::unexpected(std::move(e));
        }
        return {};
    }

    [[nodiscard]] std::expected<StmtPtr, ChainApiError> prepare(std::string_view sql) {
        sqlite3_stmt* raw = nullptr;
        const int rc =
            sqlite3_prepare_v2(db.get(), sql.data(), static_cast<int>(sql.size()), &raw, nullptr);
        if (rc != SQLITE_OK) {
            return std::unexpected(sqliteError(db.get(), "history: prepare"));
        }
        return StmtPtr{raw};
    }

    [[nodiscard]] std::expected<void, ChainApiError> ensureSchema() {
        if (auto r = exec("PRAGMA journal_mode = WAL;"); !r) return r;
        if (auto r = exec("PRAGMA foreign_keys = ON;"); !r) return r;
        if (auto r = exec("PRAGMA synchronous = NORMAL;"); !r) return r;

        const char* createSchema = R"SQL(
            CREATE TABLE IF NOT EXISTS schema_version (
                version INTEGER PRIMARY KEY
            );
            CREATE TABLE IF NOT EXISTS runs (
                run_id      INTEGER PRIMARY KEY,
                target_op   TEXT,
                env_name    TEXT,
                started_at  TEXT,
                ended_at    TEXT,
                outcome     TEXT,
                chain_size  INTEGER
            );
            CREATE TABLE IF NOT EXISTS run_events (
                id          INTEGER PRIMARY KEY AUTOINCREMENT,
                run_id      INTEGER NOT NULL,
                seq         INTEGER NOT NULL,
                event_type  TEXT NOT NULL,
                step_index  INTEGER,
                op_id       TEXT,
                payload     TEXT NOT NULL,
                at          TEXT NOT NULL,
                FOREIGN KEY (run_id) REFERENCES runs(run_id) ON DELETE CASCADE
            );
            CREATE INDEX IF NOT EXISTS idx_run_events_run_seq
                ON run_events(run_id, seq);
            CREATE INDEX IF NOT EXISTS idx_runs_started_at
                ON runs(started_at DESC);
        )SQL";
        if (auto r = exec(createSchema); !r) return r;

        // v1 is the only version today; future migrations check this
        // row before applying ALTERs and bump it.
        if (auto r = exec("INSERT OR IGNORE INTO schema_version(version) VALUES (1);"); !r)
            return r;

        return {};
    }

    [[nodiscard]] std::expected<void, ChainApiError> prepareStatements() {
        auto p1 = prepare(
            "INSERT INTO run_events"
            "  (run_id, seq, event_type, step_index, op_id, payload, at) "
            "VALUES (?, ?, ?, ?, ?, ?, ?);");
        if (!p1) return std::unexpected(p1.error());
        insertEventStmt = std::move(*p1);

        auto p2 = prepare(
            "INSERT OR IGNORE INTO runs"
            "  (run_id, target_op, env_name, started_at, chain_size) "
            "VALUES (?, ?, ?, ?, ?);");
        if (!p2) return std::unexpected(p2.error());
        insertRunStmt = std::move(*p2);

        auto p3 = prepare(
            "UPDATE runs SET target_op = ?, env_name = ?, "
            "started_at = ?, chain_size = ? WHERE run_id = ?;");
        if (!p3) return std::unexpected(p3.error());
        updateRunStartedStmt = std::move(*p3);

        auto p4 = prepare("UPDATE runs SET ended_at = ?, outcome = ? WHERE run_id = ?;");
        if (!p4) return std::unexpected(p4.error());
        updateRunEndedStmt = std::move(*p4);

        auto p5 = prepare("SELECT COALESCE(MAX(seq), 0) + 1 FROM run_events WHERE run_id = ?;");
        if (!p5) return std::unexpected(p5.error());
        nextSeqStmt = std::move(*p5);

        return {};
    }

    [[nodiscard]] std::int64_t nextSeq(std::uint64_t runId) {
        sqlite3_reset(nextSeqStmt.get());
        sqlite3_bind_int64(nextSeqStmt.get(), 1, static_cast<sqlite3_int64>(runId));
        if (sqlite3_step(nextSeqStmt.get()) != SQLITE_ROW) return 1;
        return sqlite3_column_int64(nextSeqStmt.get(), 0);
    }
};

// ─── Public API ──────────────────────────────────────────────────────────────

SqliteHistoryStore::SqliteHistoryStore() : impl_(std::make_unique<Impl>()) {}
SqliteHistoryStore::~SqliteHistoryStore() = default;

std::expected<void, ChainApiError> SqliteHistoryStore::open(const fs::path& dbPath) {
    // Make sure the parent dir exists; sqlite3_open won't create it.
    if (dbPath.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(dbPath.parent_path(), ec);
        if (ec) {
            return std::unexpected(
                ChainApiError{ErrorCode::SchemaInvalid,
                              ErrorClass::Schema,
                              "history: cannot create parent dir: " + ec.message()});
        }
    }

    sqlite3* raw = nullptr;
    const auto pathStr = dbPath.string();
    const int rc = sqlite3_open_v2(pathStr.c_str(),
                                   &raw,
                                   SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                                   nullptr);
    if (rc != SQLITE_OK) {
        ChainApiError e = sqliteError(raw, "history: open");
        sqlite3_close(raw);
        return std::unexpected(std::move(e));
    }
    impl_->db.reset(raw);
    sqlite3_busy_timeout(impl_->db.get(), kBusyTimeoutMs);

    if (auto r = impl_->ensureSchema(); !r) return r;
    if (auto r = impl_->prepareStatements(); !r) return r;
    return {};
}

std::expected<void, ChainApiError> SqliteHistoryStore::append(const RunEvent& event) {
    if (!impl_->db) {
        return std::unexpected(ChainApiError{
            ErrorCode::SchemaInvalid, ErrorClass::Schema, "history: append before open"});
    }

    const auto env = envelopeOf(event);

    // All three statements below (run row + optional run-ended update +
    // event insert) commit as one unit. Without this each sqlite3_step ran
    // in its own implicit transaction — 3 fsyncs per event and a window
    // where the event row could land without its parent run row.
    SqliteTransaction txn{impl_->db.get()};
    if (!txn.began()) {
        return std::unexpected(sqliteError(impl_->db.get(), "history: begin append txn"));
    }

    // RunStarted creates the runs row; RunEnded stamps the terminal
    // state; any other event backfills a placeholder row so the FK
    // holds even if events arrive out of order.
    if (env.eventType == "RunStarted") {
        sqlite3_reset(impl_->insertRunStmt.get());
        sqlite3_bind_int64(impl_->insertRunStmt.get(), 1, static_cast<sqlite3_int64>(env.runId));
        const auto target = env.payload.value("target", std::string{});
        const auto envName = env.payload.value("envName", std::string{});
        const auto chainSize = env.payload.value("chainSize", std::size_t{0});
        sqlite3_bind_text(impl_->insertRunStmt.get(), 2, target.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(impl_->insertRunStmt.get(), 3, envName.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(impl_->insertRunStmt.get(), 4, env.at.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(impl_->insertRunStmt.get(), 5, static_cast<sqlite3_int64>(chainSize));
        if (sqlite3_step(impl_->insertRunStmt.get()) != SQLITE_DONE) {
            return std::unexpected(sqliteError(impl_->db.get(), "history: insert run"));
        }
        // INSERT OR IGNORE won't touch an existing row, so UPDATE the
        // descriptive columns from the canonical RunStarted payload.
        sqlite3_reset(impl_->updateRunStartedStmt.get());
        sqlite3_bind_text(
            impl_->updateRunStartedStmt.get(), 1, target.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(
            impl_->updateRunStartedStmt.get(), 2, envName.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(
            impl_->updateRunStartedStmt.get(), 3, env.at.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(
            impl_->updateRunStartedStmt.get(), 4, static_cast<sqlite3_int64>(chainSize));
        sqlite3_bind_int64(
            impl_->updateRunStartedStmt.get(), 5, static_cast<sqlite3_int64>(env.runId));
        if (sqlite3_step(impl_->updateRunStartedStmt.get()) != SQLITE_DONE) {
            return std::unexpected(sqliteError(impl_->db.get(), "history: update run start"));
        }
    } else {
        // Backfill a parent row so the event insert's FK holds when
        // events arrive out of order (test fixtures do this).
        sqlite3_reset(impl_->insertRunStmt.get());
        sqlite3_bind_int64(impl_->insertRunStmt.get(), 1, static_cast<sqlite3_int64>(env.runId));
        sqlite3_bind_null(impl_->insertRunStmt.get(), 2);
        sqlite3_bind_null(impl_->insertRunStmt.get(), 3);
        sqlite3_bind_null(impl_->insertRunStmt.get(), 4);
        sqlite3_bind_int64(impl_->insertRunStmt.get(), 5, 0);
        if (sqlite3_step(impl_->insertRunStmt.get()) != SQLITE_DONE) {
            return std::unexpected(sqliteError(impl_->db.get(), "history: backfill run"));
        }
    }

    if (env.eventType == "RunEnded") {
        sqlite3_reset(impl_->updateRunEndedStmt.get());
        sqlite3_bind_text(impl_->updateRunEndedStmt.get(), 1, env.at.c_str(), -1, SQLITE_TRANSIENT);
        const auto outcome = env.payload.value("outcome", std::string{"Failed"});
        sqlite3_bind_text(
            impl_->updateRunEndedStmt.get(), 2, outcome.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(
            impl_->updateRunEndedStmt.get(), 3, static_cast<sqlite3_int64>(env.runId));
        if (sqlite3_step(impl_->updateRunEndedStmt.get()) != SQLITE_DONE) {
            return std::unexpected(sqliteError(impl_->db.get(), "history: update run end"));
        }
    }

    const auto seq = impl_->nextSeq(env.runId);
    const auto payloadStr = env.payload.dump();

    sqlite3_reset(impl_->insertEventStmt.get());
    sqlite3_bind_int64(impl_->insertEventStmt.get(), 1, static_cast<sqlite3_int64>(env.runId));
    sqlite3_bind_int64(impl_->insertEventStmt.get(), 2, seq);
    sqlite3_bind_text(impl_->insertEventStmt.get(), 3, env.eventType.c_str(), -1, SQLITE_TRANSIENT);
    if (env.stepIndex) {
        sqlite3_bind_int64(
            impl_->insertEventStmt.get(), 4, static_cast<sqlite3_int64>(*env.stepIndex));
    } else {
        sqlite3_bind_null(impl_->insertEventStmt.get(), 4);
    }
    if (env.opId) {
        sqlite3_bind_text(impl_->insertEventStmt.get(), 5, env.opId->c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(impl_->insertEventStmt.get(), 5);
    }
    sqlite3_bind_text(impl_->insertEventStmt.get(), 6, payloadStr.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(impl_->insertEventStmt.get(), 7, env.at.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(impl_->insertEventStmt.get()) != SQLITE_DONE) {
        return std::unexpected(sqliteError(impl_->db.get(), "history: insert event"));
    }
    if (!txn.commit()) {
        return std::unexpected(sqliteError(impl_->db.get(), "history: commit append txn"));
    }
    return {};
}

std::expected<std::vector<RunEvent>, ChainApiError> SqliteHistoryStore::eventsFor(RunId run) const {
    if (!impl_->db) {
        return std::unexpected(ChainApiError{
            ErrorCode::SchemaInvalid, ErrorClass::Schema, "history: eventsFor before open"});
    }

    sqlite3_stmt* raw = nullptr;
    const char* sql =
        "SELECT event_type, step_index, payload, at "
        "FROM run_events WHERE run_id = ? ORDER BY seq ASC;";
    if (sqlite3_prepare_v2(impl_->db.get(), sql, -1, &raw, nullptr) != SQLITE_OK) {
        return std::unexpected(sqliteError(impl_->db.get(), "history: prepare eventsFor"));
    }
    StmtPtr stmt{raw};

    sqlite3_bind_int64(stmt.get(), 1, static_cast<sqlite3_int64>(run.value));

    std::vector<RunEvent> out;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        const auto* eventType = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
        const bool stepIndexNull = (sqlite3_column_type(stmt.get(), 1) == SQLITE_NULL);
        const auto stepIndex = stepIndexNull ? std::optional<std::size_t>{}
                                             : std::optional<std::size_t>{static_cast<std::size_t>(
                                                   sqlite3_column_int64(stmt.get(), 1))};
        const auto* payloadStr = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
        const auto* atStr = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 3));

        json payload;
        try {
            payload = json::parse(payloadStr != nullptr ? payloadStr : "{}");
        } catch (const json::parse_error&) {
            // Skip a corrupt row rather than failing the whole replay.
            continue;
        }
        if (auto ev = eventFromRow(eventType != nullptr ? eventType : "",
                                   run.value,
                                   stepIndex,
                                   atStr != nullptr ? atStr : "",
                                   payload)) {
            out.push_back(std::move(*ev));
        }
    }
    return out;
}

std::expected<std::vector<RunHistoryRow>, ChainApiError> SqliteHistoryStore::listRuns(
    std::size_t limit) const {
    if (!impl_->db) {
        return std::unexpected(ChainApiError{
            ErrorCode::SchemaInvalid, ErrorClass::Schema, "history: listRuns before open"});
    }

    std::string sql =
        "SELECT run_id, COALESCE(target_op, ''), COALESCE(env_name, ''), "
        "       COALESCE(started_at, ''), COALESCE(ended_at, ''), "
        "       COALESCE(outcome, ''), COALESCE(chain_size, 0) "
        "FROM runs ORDER BY started_at DESC, run_id DESC";
    if (limit > 0) {
        sql += " LIMIT " + std::to_string(limit);
    }
    sql += ";";

    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(impl_->db.get(), sql.c_str(), -1, &raw, nullptr) != SQLITE_OK) {
        return std::unexpected(sqliteError(impl_->db.get(), "history: prepare listRuns"));
    }
    StmtPtr stmt{raw};

    std::vector<RunHistoryRow> out;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        RunHistoryRow row;
        row.runId = RunId{static_cast<std::uint64_t>(sqlite3_column_int64(stmt.get(), 0))};
        row.targetOp =
            OperationId{reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1))};
        row.envName = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
        row.startedAt = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 3));
        row.endedAt = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 4));
        row.outcome = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 5));
        row.chainSize = static_cast<std::size_t>(sqlite3_column_int64(stmt.get(), 6));
        out.push_back(std::move(row));
    }
    return out;
}

void SqliteHistoryStore::close() {
    impl_->insertEventStmt.reset();
    impl_->insertRunStmt.reset();
    impl_->updateRunStartedStmt.reset();
    impl_->updateRunEndedStmt.reset();
    impl_->nextSeqStmt.reset();
    impl_->db.reset();
}

}  // namespace chainapi::engine
