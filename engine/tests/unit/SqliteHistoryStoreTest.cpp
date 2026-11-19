// SqliteHistoryStoreTest — confirms each RunEvent variant round-trips
// through SQLite, the runs table denormalises RunStarted/RunEnded
// correctly, and the schema survives close-and-reopen against the same
// file (every desktop launch must observe the prior run history).
//
// The store implementation lives under `engine/src/`; the tests reach
// into it via the private include path that `engine/tests/CMakeLists.txt`
// already adds. No public API change is needed — the desktop and CLI
// see the store through `HistoryStore` and `makeSqliteHistoryStore()`.

#include "infrastructure/storage/SqliteHistoryStore.h"

#include <reqloom/engine/Events.h>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <support/TempPath.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace ce = reqloom::engine;
namespace fs = std::filesystem;

namespace {

class TempDb {
public:
    TempDb() { path_ = reqloom::tests::uniqueTempPath("reqloom-history", ".sqlite"); }
    ~TempDb() {
        std::error_code ec;
        fs::remove(path_, ec);
        // SQLite WAL mode leaves -wal and -shm sidecar files alongside.
        fs::remove(fs::path{path_.string() + "-wal"}, ec);
        fs::remove(fs::path{path_.string() + "-shm"}, ec);
    }
    TempDb(const TempDb&) = delete;
    TempDb& operator=(const TempDb&) = delete;

    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

[[nodiscard]] ce::TimePoint someTimePoint() {
    // Fixed instant — round-tripping via ISO-8601 truncates sub-second
    // precision, so use a value that has none to start with.
    using namespace std::chrono;
    return system_clock::time_point{seconds{1748352000}};  // 2025-05-27T12:00:00Z
}

void insertLegacyDiagnosticRows(const fs::path& path) {
    sqlite3* rawDatabase{};
    const int openResult = sqlite3_open(path.string().c_str(), &rawDatabase);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database{rawDatabase, &sqlite3_close};
    ASSERT_EQ(openResult, SQLITE_OK);

    constexpr const char* kSql = R"SQL(
        INSERT INTO runs (run_id, target_op, started_at) VALUES
            (23, 'legacy.get', '2025-05-27T12:00:00Z');
        INSERT INTO run_events
            (run_id, seq, event_type, step_index, op_id, payload, at) VALUES
            (23, 0, 'StepFailed', 0, 'legacy.get',
             '{"op":"legacy.get","code":"E_VAR_UNRESOLVED","attempt":1,"detail":"legacy"}',
             '2025-05-27T12:00:00Z'),
            (23, 1, 'StepFailed', 1, 'legacy.get',
             '{
                "op":"legacy.get",
                "code":"E_VAR_UNRESOLVED",
                "attempt":1,
                "detail":"future",
                "diagnostics":[{
                    "token":"future.token",
                    "useKind":"FutureUse",
                    "useName":"field",
                    "cause":"FutureCause",
                    "sourceKind":"FutureSource",
                    "sourceId":"source",
                    "sourceField":"value"
                }]
             }',
             '2025-05-27T12:00:00Z');
    )SQL";
    char* rawError{};
    const int result = sqlite3_exec(database.get(), kSql, nullptr, nullptr, &rawError);
    const std::string error = rawError != nullptr ? rawError : "";
    sqlite3_free(rawError);
    ASSERT_EQ(result, SQLITE_OK) << error;
}

[[nodiscard]] ce::StepFailed diagnosticFailure() {
    ce::StepFailed failure;
    failure.runId = ce::RunId{9};
    failure.stepIndex = 1;
    failure.op = ce::OperationId{"x.y"};
    failure.code = ce::ErrorCode::VarUnresolved;
    failure.cls = ce::classify(ce::ErrorCode::VarUnresolved);
    failure.attempt = 3;
    failure.detail = "variable unavailable";
    failure.at = someTimePoint();

    ce::UnresolvedVariableDiagnostic environment;
    environment.token = "env.ORDER_ID";
    environment.useKind = ce::VariableUseKind::UrlPath;
    environment.cause = ce::UnresolvedVariableCause::EnvironmentValueMissing;
    environment.sourceKind = ce::VariableSourceKind::Environment;
    environment.sourceId = "local";
    environment.sourceField = "ORDER_ID";
    failure.diagnostics.push_back(environment);

    ce::UnresolvedVariableDiagnostic extraction;
    extraction.token = "order.id";
    extraction.useKind = ce::VariableUseKind::Auth;
    extraction.useName = "Authorization";
    extraction.cause = ce::UnresolvedVariableCause::ExtractionNull;
    extraction.sourceKind = ce::VariableSourceKind::Extraction;
    extraction.sourceId = "order";
    extraction.sourceField = "id";
    extraction.producerOp = ce::OperationId{"order.create"};
    extraction.producerStepIndex = 0;
    failure.diagnostics.push_back(extraction);
    return failure;
}

void expectDiagnosticFailure(const ce::StepFailed& failure) {
    EXPECT_EQ(failure.code, ce::ErrorCode::VarUnresolved);
    EXPECT_EQ(failure.cls, ce::ErrorClass::Resolution);
    EXPECT_EQ(failure.attempt, 3);
    EXPECT_EQ(failure.detail, "variable unavailable");
    ASSERT_EQ(failure.diagnostics.size(), 2u);
    EXPECT_EQ(failure.diagnostics[0].token, "env.ORDER_ID");
    EXPECT_EQ(failure.diagnostics[0].useKind, ce::VariableUseKind::UrlPath);
    EXPECT_EQ(failure.diagnostics[0].cause, ce::UnresolvedVariableCause::EnvironmentValueMissing);
    EXPECT_FALSE(failure.diagnostics[0].producerOp.has_value());
    EXPECT_EQ(failure.diagnostics[1].useKind, ce::VariableUseKind::Auth);
    EXPECT_EQ(failure.diagnostics[1].useName, "Authorization");
    EXPECT_EQ(failure.diagnostics[1].cause, ce::UnresolvedVariableCause::ExtractionNull);
    EXPECT_EQ(failure.diagnostics[1].sourceKind, ce::VariableSourceKind::Extraction);
    ASSERT_TRUE(failure.diagnostics[1].producerOp.has_value());
    EXPECT_EQ(failure.diagnostics[1].producerOp->value, "order.create");
    EXPECT_EQ(failure.diagnostics[1].producerStepIndex, 0u);
}

}  // namespace

// ─── Lifecycle ───────────────────────────────────────────────────────────────

TEST(SqliteHistoryStore, open_creates_database_file) {
    TempDb tmp;
    ASSERT_FALSE(fs::exists(tmp.path()));

    ce::SqliteHistoryStore store;
    auto opened = store.open(tmp.path());
    ASSERT_TRUE(opened.has_value()) << opened.error().detail;
    EXPECT_TRUE(fs::exists(tmp.path()));
    store.close();
}

TEST(SqliteHistoryStore, open_creates_parent_directory_if_missing) {
    // The desktop puts the history db at ~/Library/Application Support/...
    // which often doesn't exist on a fresh install. Open must create
    // the parent rather than failing with "no such file or directory".
    const auto root = reqloom::tests::uniqueTempPath("reqloom-history-mkdir");
    const auto nested = root / "deep" / "history.sqlite";
    std::error_code ec;
    fs::remove_all(root, ec);

    ce::SqliteHistoryStore store;
    auto opened = store.open(nested);
    ASSERT_TRUE(opened.has_value()) << opened.error().detail;
    EXPECT_TRUE(fs::exists(nested));

    store.close();
    fs::remove_all(root, ec);
}

TEST(SqliteHistoryStore, append_before_open_returns_error) {
    ce::SqliteHistoryStore store;
    ce::RunStarted ev;
    ev.runId = ce::RunId{1};
    ev.target = ce::OperationId{"x.y"};
    ev.at = someTimePoint();

    auto result = store.append(ev);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().detail.find("before open"), std::string::npos);
}

// ─── Round-trip: every event variant ────────────────────────────────────────

TEST(SqliteHistoryStore, run_started_and_ended_round_trip_via_eventsFor) {
    TempDb tmp;
    ce::SqliteHistoryStore store;
    ASSERT_TRUE(store.open(tmp.path()).has_value());

    ce::RunStarted started;
    started.runId = ce::RunId{42};
    started.target = ce::OperationId{"order.create"};
    started.chainSize = 3;
    started.envName = "local";
    started.at = someTimePoint();
    ASSERT_TRUE(store.append(started).has_value());

    ce::RunEnded ended;
    ended.runId = ce::RunId{42};
    ended.outcome = ce::RunOutcome::Succeeded;
    ended.elapsed = std::chrono::milliseconds{2500};
    ended.at = someTimePoint() + std::chrono::seconds{2};
    ASSERT_TRUE(store.append(ended).has_value());

    auto replayed = store.eventsFor(ce::RunId{42});
    ASSERT_TRUE(replayed.has_value()) << replayed.error().detail;
    ASSERT_EQ(replayed->size(), 2u);

    const auto* startedBack = std::get_if<ce::RunStarted>(&(*replayed)[0]);
    ASSERT_NE(startedBack, nullptr);
    EXPECT_EQ(startedBack->target.value, "order.create");
    EXPECT_EQ(startedBack->chainSize, 3u);
    EXPECT_EQ(startedBack->envName, "local");

    const auto* endedBack = std::get_if<ce::RunEnded>(&(*replayed)[1]);
    ASSERT_NE(endedBack, nullptr);
    EXPECT_EQ(endedBack->outcome, ce::RunOutcome::Succeeded);
    EXPECT_EQ(endedBack->elapsed, std::chrono::milliseconds{2500});
}

TEST(SqliteHistoryStore, request_response_extraction_events_round_trip) {
    TempDb tmp;
    ce::SqliteHistoryStore store;
    ASSERT_TRUE(store.open(tmp.path()).has_value());

    ce::RunId rid{7};
    ce::RunStarted rs;
    rs.runId = rid;
    rs.target = ce::OperationId{"x.y"};
    rs.at = someTimePoint();
    store.append(rs);

    ce::RequestPrepared req;
    req.runId = rid;
    req.stepIndex = 2;
    req.method = ce::HttpMethod::Post;
    req.url = "https://api.example.com/orders";
    req.maskedHeaders = {{"Authorization", std::string{ce::kRedactedHeaderValue}},
                         {"Content-Type", "application/json"}};
    req.bodySize = 128;
    req.at = someTimePoint();
    ASSERT_TRUE(store.append(req).has_value());

    ce::ResponseReceived resp;
    resp.runId = rid;
    resp.stepIndex = 2;
    resp.status = 201;
    resp.headers = {{"Set-Cookie", std::string{ce::kRedactedHeaderValue}}};
    resp.bodySize = 56;
    resp.elapsed = std::chrono::milliseconds{42};
    resp.at = someTimePoint();
    ASSERT_TRUE(store.append(resp).has_value());

    ce::ExtractionApplied ext;
    ext.runId = rid;
    ext.stepIndex = 2;
    ext.resource = ce::ResourceId{"order"};
    ext.variableNames = {"id", "ts"};
    ext.at = someTimePoint();
    ASSERT_TRUE(store.append(ext).has_value());

    auto replayed = store.eventsFor(rid);
    ASSERT_TRUE(replayed.has_value());
    ASSERT_EQ(replayed->size(), 4u);  // RunStarted + 3 events

    const auto* reqBack = std::get_if<ce::RequestPrepared>(&(*replayed)[1]);
    ASSERT_NE(reqBack, nullptr);
    EXPECT_EQ(reqBack->method, ce::HttpMethod::Post);
    EXPECT_EQ(reqBack->url, "https://api.example.com/orders");
    EXPECT_EQ(reqBack->bodySize, 128u);
    ASSERT_EQ(reqBack->maskedHeaders.size(), 2u);
    EXPECT_EQ(reqBack->maskedHeaders[0].first, "Authorization");
    EXPECT_EQ(reqBack->maskedHeaders[0].second, ce::kRedactedHeaderValue);

    const auto* respBack = std::get_if<ce::ResponseReceived>(&(*replayed)[2]);
    ASSERT_NE(respBack, nullptr);
    EXPECT_EQ(respBack->status, 201);
    EXPECT_EQ(respBack->bodySize, 56u);
    EXPECT_EQ(respBack->elapsed, std::chrono::milliseconds{42});

    const auto* extBack = std::get_if<ce::ExtractionApplied>(&(*replayed)[3]);
    ASSERT_NE(extBack, nullptr);
    EXPECT_EQ(extBack->resource.value, "order");
    ASSERT_EQ(extBack->variableNames.size(), 2u);
    EXPECT_EQ(extBack->variableNames[0], "id");
    EXPECT_EQ(extBack->variableNames[1], "ts");
}

TEST(SqliteHistoryStore, exchange_kind_and_assertion_detail_round_trip) {
    TempDb tmp;
    ce::SqliteHistoryStore store;
    ASSERT_TRUE(store.open(tmp.path()).has_value());
    const ce::RunId rid{11};
    ce::RunStarted rs;
    rs.runId = rid;
    rs.target = ce::OperationId{"x.y"};
    rs.at = someTimePoint();
    store.append(rs);

    ce::RequestPrepared req;
    req.runId = rid;
    req.at = someTimePoint();
    req.exchange = ce::ExchangeKind::SignIn;
    ASSERT_TRUE(store.append(req).has_value());
    ce::ResponseReceived resp;
    resp.runId = rid;
    resp.at = someTimePoint();
    resp.exchange = ce::ExchangeKind::SessionRefresh;
    ASSERT_TRUE(store.append(resp).has_value());
    ce::ResponseReceived own;
    own.runId = rid;
    own.at = someTimePoint();
    ASSERT_TRUE(store.append(own).has_value());
    ce::AssertionCompleted assertion;
    assertion.runId = rid;
    assertion.name = "paid";
    assertion.expr = "$.status == \"paid\"";
    assertion.at = someTimePoint();
    assertion.detail = "$.status was \"pending\", expected == \"paid\"";
    ASSERT_TRUE(store.append(assertion).has_value());

    const auto replayed = store.eventsFor(rid);
    ASSERT_TRUE(replayed.has_value());
    ASSERT_EQ(replayed->size(), 5u);
    EXPECT_EQ(std::get<ce::RequestPrepared>((*replayed)[1]).exchange, ce::ExchangeKind::SignIn);
    EXPECT_EQ(std::get<ce::ResponseReceived>((*replayed)[2]).exchange,
              ce::ExchangeKind::SessionRefresh);
    EXPECT_EQ(std::get<ce::ResponseReceived>((*replayed)[3]).exchange, ce::ExchangeKind::Operation);
    EXPECT_EQ(std::get<ce::AssertionCompleted>((*replayed)[4]).detail, assertion.detail);
}

TEST(SqliteHistoryStore, rows_written_before_exchange_kinds_read_as_the_operations_own) {
    TempDb tmp;
    {
        ce::SqliteHistoryStore schemaCreator;
        ASSERT_TRUE(schemaCreator.open(tmp.path()).has_value());
        schemaCreator.close();
    }
    {
        sqlite3* rawDatabase{};
        ASSERT_EQ(sqlite3_open(tmp.path().string().c_str(), &rawDatabase), SQLITE_OK);
        std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database{rawDatabase, &sqlite3_close};
        constexpr const char* kSql = R"SQL(
            INSERT INTO runs (run_id, target_op, started_at) VALUES
                (24, 'old.get', '2025-05-27T12:00:00Z');
            INSERT INTO run_events
                (run_id, seq, event_type, step_index, op_id, payload, at) VALUES
                (24, 0, 'RequestPrepared', 0, NULL,
                 '{"method":"GET","url":"u","headers":[],"bodySize":0}', '2025-05-27T12:00:00Z'),
                (24, 1, 'ResponseReceived', 0, NULL,
                 '{"status":200,"headers":[],"bodySize":0,"elapsedMs":1}', '2025-05-27T12:00:00Z'),
                (24, 2, 'ResponseReceived', 0, NULL,
                 '{"status":200,"headers":[],"bodySize":0,"elapsedMs":1,"exchange":"from_the_future"}',
                 '2025-05-27T12:00:00Z'),
                (24, 3, 'AssertionCompleted', 0, 'old.get',
                 '{"op":"old.get","name":"n","expr":"$.a","passed":false}', '2025-05-27T12:00:00Z');
        )SQL";
        ASSERT_EQ(sqlite3_exec(database.get(), kSql, nullptr, nullptr, nullptr), SQLITE_OK);
    }

    ce::SqliteHistoryStore store;
    ASSERT_TRUE(store.open(tmp.path()).has_value());
    const auto replayed = store.eventsFor(ce::RunId{24});
    ASSERT_TRUE(replayed.has_value());
    ASSERT_EQ(replayed->size(), 4u);
    EXPECT_EQ(std::get<ce::RequestPrepared>((*replayed)[0]).exchange, ce::ExchangeKind::Operation);
    EXPECT_EQ(std::get<ce::ResponseReceived>((*replayed)[1]).exchange, ce::ExchangeKind::Operation);
    EXPECT_EQ(std::get<ce::ResponseReceived>((*replayed)[2]).exchange, ce::ExchangeKind::Operation);
    EXPECT_TRUE(std::get<ce::AssertionCompleted>((*replayed)[3]).detail.empty());
}

TEST(SqliteHistoryStore, captured_response_body_round_trips) {
    // When a run opts into body capture, the body must survive a replay
    // so the desktop can show past responses in full (Postman-style
    // history). A row without a captured body must replay with an empty
    // optional, not an empty string.
    TempDb tmp;
    ce::SqliteHistoryStore store;
    ASSERT_TRUE(store.open(tmp.path()).has_value());

    const ce::RunId rid{11};
    ce::RunStarted rs;
    rs.runId = rid;
    rs.target = ce::OperationId{"order.create"};
    rs.at = someTimePoint();
    store.append(rs);

    const std::string payload = R"({"data":{"id":"ord-1","status":"confirmed"}})";

    ce::ResponseReceived captured;
    captured.runId = rid;
    captured.stepIndex = 0;
    captured.status = 201;
    captured.bodySize = payload.size();
    captured.elapsed = std::chrono::milliseconds{17};
    captured.body = payload;
    captured.at = someTimePoint();
    ASSERT_TRUE(store.append(captured).has_value());

    ce::ResponseReceived uncaptured;
    uncaptured.runId = rid;
    uncaptured.stepIndex = 1;
    uncaptured.status = 200;
    uncaptured.bodySize = 56;
    uncaptured.elapsed = std::chrono::milliseconds{9};
    uncaptured.at = someTimePoint();
    ASSERT_TRUE(store.append(uncaptured).has_value());

    auto replayed = store.eventsFor(rid);
    ASSERT_TRUE(replayed.has_value());
    ASSERT_EQ(replayed->size(), 3u);  // RunStarted + 2 responses

    const auto* capturedBack = std::get_if<ce::ResponseReceived>(&(*replayed)[1]);
    ASSERT_NE(capturedBack, nullptr);
    ASSERT_TRUE(capturedBack->body.has_value());
    EXPECT_EQ(*capturedBack->body, payload);

    const auto* uncapturedBack = std::get_if<ce::ResponseReceived>(&(*replayed)[2]);
    ASSERT_NE(uncapturedBack, nullptr);
    EXPECT_FALSE(uncapturedBack->body.has_value());
}

TEST(SqliteHistoryStore, captured_binary_body_round_trips_without_throwing) {
    // Captured bodies are arbitrary bytes (images, gzip, protobuf). A raw
    // non-UTF-8 string would make nlohmann's strict dump() throw; the store
    // base64-encodes the body so append() succeeds and replay restores the
    // exact bytes. This is the regression guard for that path.
    TempDb tmp;
    ce::SqliteHistoryStore store;
    ASSERT_TRUE(store.open(tmp.path()).has_value());

    const ce::RunId rid{12};
    ce::RunStarted rs;
    rs.runId = rid;
    rs.target = ce::OperationId{"blob.get"};
    rs.at = someTimePoint();
    store.append(rs);

    // Every byte value 0x00..0xFF — deliberately invalid UTF-8.
    std::string binary;
    binary.reserve(256);
    for (int i = 0; i < 256; ++i) {
        binary.push_back(static_cast<char>(i));
    }

    ce::ResponseReceived resp;
    resp.runId = rid;
    resp.stepIndex = 0;
    resp.status = 200;
    resp.bodySize = binary.size();
    resp.body = binary;
    resp.at = someTimePoint();
    ASSERT_TRUE(store.append(resp).has_value());

    auto replayed = store.eventsFor(rid);
    ASSERT_TRUE(replayed.has_value());
    ASSERT_EQ(replayed->size(), 2u);

    const auto* back = std::get_if<ce::ResponseReceived>(&(*replayed)[1]);
    ASSERT_NE(back, nullptr);
    ASSERT_TRUE(back->body.has_value());
    EXPECT_EQ(*back->body, binary);
}

TEST(SqliteHistoryStore, step_failed_diagnostics_round_trip_with_optional_producer) {
    TempDb tmp;
    ce::SqliteHistoryStore store;
    ASSERT_TRUE(store.open(tmp.path()).has_value());

    ce::RunStarted started;
    started.runId = ce::RunId{9};
    started.target = ce::OperationId{"x.y"};
    started.at = someTimePoint();
    ASSERT_TRUE(store.append(started).has_value());
    ASSERT_TRUE(store.append(diagnosticFailure()).has_value());

    const auto replayed = store.eventsFor(ce::RunId{9});
    ASSERT_TRUE(replayed.has_value());
    ASSERT_EQ(replayed->size(), 2u);

    const auto* failure = std::get_if<ce::StepFailed>(&(*replayed)[1]);
    ASSERT_NE(failure, nullptr);
    expectDiagnosticFailure(*failure);
}

TEST(SqliteHistoryStore, legacy_and_unknown_diagnostics_replay_conservatively) {
    TempDb tmp;
    {
        ce::SqliteHistoryStore schemaCreator;
        ASSERT_TRUE(schemaCreator.open(tmp.path()).has_value());
        schemaCreator.close();
    }
    insertLegacyDiagnosticRows(tmp.path());

    ce::SqliteHistoryStore store;
    ASSERT_TRUE(store.open(tmp.path()).has_value());
    const auto replayed = store.eventsFor(ce::RunId{23});
    ASSERT_TRUE(replayed.has_value());
    ASSERT_EQ(replayed->size(), 2u);

    const auto* legacy = std::get_if<ce::StepFailed>(&(*replayed)[0]);
    ASSERT_NE(legacy, nullptr);
    EXPECT_TRUE(legacy->diagnostics.empty());

    const auto* future = std::get_if<ce::StepFailed>(&(*replayed)[1]);
    ASSERT_NE(future, nullptr);
    ASSERT_EQ(future->diagnostics.size(), 1u);
    const auto& diagnostic = future->diagnostics.front();
    EXPECT_EQ(diagnostic.token, "future.token");
    EXPECT_EQ(diagnostic.useKind, ce::VariableUseKind::Unknown);
    EXPECT_EQ(diagnostic.cause, ce::UnresolvedVariableCause::Unavailable);
    EXPECT_EQ(diagnostic.sourceKind, ce::VariableSourceKind::Unknown);
    EXPECT_EQ(diagnostic.sourceId, "source");
    EXPECT_EQ(diagnostic.sourceField, "value");
    EXPECT_FALSE(diagnostic.producerOp.has_value());
    EXPECT_FALSE(diagnostic.producerStepIndex.has_value());
}

TEST(SqliteHistoryStore, blocked_cancelled_and_session_events_round_trip) {
    TempDb tmp;
    ce::SqliteHistoryStore store;
    ASSERT_TRUE(store.open(tmp.path()).has_value());

    ce::RunStarted rs;
    rs.runId = ce::RunId{11};
    rs.target = ce::OperationId{"x.y"};
    rs.at = someTimePoint();
    store.append(rs);

    ce::StepBlocked blocked;
    blocked.runId = ce::RunId{11};
    blocked.stepIndex = 1;
    blocked.op = ce::OperationId{"second.get"};
    blocked.blockedByStepIndex = 0;
    blocked.at = someTimePoint();
    ASSERT_TRUE(store.append(blocked).has_value());

    ce::StepCancelled sc;
    sc.runId = ce::RunId{11};
    sc.stepIndex = 0;
    sc.op = ce::OperationId{"first.get"};
    sc.at = someTimePoint();
    ASSERT_TRUE(store.append(sc).has_value());

    ce::SessionRefreshed sr;
    sr.runId = ce::RunId{11};
    sr.actor = ce::ActorId{"vendor"};
    sr.trigger = ce::SessionRefreshed::Trigger::Unauthorized;
    sr.at = someTimePoint();
    ASSERT_TRUE(store.append(sr).has_value());

    auto replayed = store.eventsFor(ce::RunId{11});
    ASSERT_TRUE(replayed.has_value());
    ASSERT_EQ(replayed->size(), 4u);

    const auto* blockedBack = std::get_if<ce::StepBlocked>(&(*replayed)[1]);
    ASSERT_NE(blockedBack, nullptr);
    EXPECT_EQ(blockedBack->stepIndex, 1u);
    EXPECT_EQ(blockedBack->op.value, "second.get");
    EXPECT_EQ(blockedBack->blockedByStepIndex, 0u);

    const auto* scBack = std::get_if<ce::StepCancelled>(&(*replayed)[2]);
    ASSERT_NE(scBack, nullptr);
    EXPECT_EQ(scBack->op.value, "first.get");

    const auto* srBack = std::get_if<ce::SessionRefreshed>(&(*replayed)[3]);
    ASSERT_NE(srBack, nullptr);
    EXPECT_EQ(srBack->actor.value, "vendor");
    EXPECT_EQ(srBack->trigger, ce::SessionRefreshed::Trigger::Unauthorized);
}

// ─── runs table denormalisation ─────────────────────────────────────────────

TEST(SqliteHistoryStore, listRuns_returns_run_with_metadata_after_run_started) {
    TempDb tmp;
    ce::SqliteHistoryStore store;
    ASSERT_TRUE(store.open(tmp.path()).has_value());

    ce::RunStarted rs;
    rs.runId = ce::RunId{100};
    rs.target = ce::OperationId{"refund.approve"};
    rs.chainSize = 9;
    rs.envName = "staging";
    rs.at = someTimePoint();
    ASSERT_TRUE(store.append(rs).has_value());

    auto rows = store.listRuns(10);
    ASSERT_TRUE(rows.has_value()) << rows.error().detail;
    ASSERT_EQ(rows->size(), 1u);

    const auto& row = (*rows)[0];
    EXPECT_EQ(row.runId.value, 100u);
    EXPECT_EQ(row.targetOp.value, "refund.approve");
    EXPECT_EQ(row.envName, "staging");
    EXPECT_EQ(row.chainSize, 9u);
    EXPECT_FALSE(row.startedAt.empty());
    // Run is still in flight — terminal columns are empty.
    EXPECT_TRUE(row.endedAt.empty());
    EXPECT_TRUE(row.outcome.empty());
}

TEST(SqliteHistoryStore, listRuns_fills_terminal_columns_after_run_ended) {
    TempDb tmp;
    ce::SqliteHistoryStore store;
    ASSERT_TRUE(store.open(tmp.path()).has_value());

    ce::RunStarted rs;
    rs.runId = ce::RunId{200};
    rs.target = ce::OperationId{"order.create"};
    rs.chainSize = 4;
    rs.envName = "local";
    rs.at = someTimePoint();
    store.append(rs);

    ce::RunEnded re;
    re.runId = ce::RunId{200};
    re.outcome = ce::RunOutcome::Failed;
    re.elapsed = std::chrono::milliseconds{1500};
    re.at = someTimePoint() + std::chrono::seconds{1};
    store.append(re);

    auto rows = store.listRuns(10);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 1u);

    const auto& row = (*rows)[0];
    EXPECT_EQ(row.outcome, "Failed");
    EXPECT_FALSE(row.endedAt.empty());
    // The precise elapsed from RunEnded is denormalised onto the run row so the
    // history view shows real durations, not a second-resolution timestamp diff.
    EXPECT_EQ(row.elapsedMs, 1500);
}

TEST(SqliteHistoryStore, listRuns_orders_runs_newest_first) {
    TempDb tmp;
    ce::SqliteHistoryStore store;
    ASSERT_TRUE(store.open(tmp.path()).has_value());

    // Two runs with distinct started_at; the newer one should sort first.
    ce::RunStarted older;
    older.runId = ce::RunId{1};
    older.target = ce::OperationId{"a.b"};
    older.at = someTimePoint();
    store.append(older);

    ce::RunStarted newer;
    newer.runId = ce::RunId{2};
    newer.target = ce::OperationId{"c.d"};
    newer.at = someTimePoint() + std::chrono::seconds{60};
    store.append(newer);

    auto rows = store.listRuns(10);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 2u);
    EXPECT_EQ((*rows)[0].runId.value, 2u);
    EXPECT_EQ((*rows)[1].runId.value, 1u);
}

TEST(SqliteHistoryStore, listRuns_honours_limit) {
    TempDb tmp;
    ce::SqliteHistoryStore store;
    ASSERT_TRUE(store.open(tmp.path()).has_value());

    for (std::uint64_t i = 1; i <= 5; ++i) {
        ce::RunStarted rs;
        rs.runId = ce::RunId{i};
        rs.target = ce::OperationId{"x.y"};
        rs.at = someTimePoint() + std::chrono::seconds{i};
        store.append(rs);
    }

    auto rows = store.listRuns(3);
    ASSERT_TRUE(rows.has_value());
    EXPECT_EQ(rows->size(), 3u);
}

// ─── Persistence across close/reopen ────────────────────────────────────────

TEST(SqliteHistoryStore, history_survives_close_and_reopen) {
    // Every desktop launch must observe the prior run history; the
    // store is a file on disk and must round-trip through process
    // exit. This is the contract the desktop history pane relies on.
    TempDb tmp;
    {
        ce::SqliteHistoryStore store;
        ASSERT_TRUE(store.open(tmp.path()).has_value());

        ce::RunStarted rs;
        rs.runId = ce::RunId{500};
        rs.target = ce::OperationId{"pay.do"};
        rs.envName = "prod";
        rs.at = someTimePoint();
        store.append(rs);

        ce::RunEnded re;
        re.runId = ce::RunId{500};
        re.outcome = ce::RunOutcome::Succeeded;
        re.at = someTimePoint() + std::chrono::seconds{3};
        store.append(re);

        store.close();
    }

    // Fresh instance, same file.
    ce::SqliteHistoryStore reopened;
    ASSERT_TRUE(reopened.open(tmp.path()).has_value());

    auto rows = reopened.listRuns(10);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 1u);
    EXPECT_EQ((*rows)[0].runId.value, 500u);
    EXPECT_EQ((*rows)[0].outcome, "Succeeded");

    auto events = reopened.eventsFor(ce::RunId{500});
    ASSERT_TRUE(events.has_value());
    EXPECT_EQ(events->size(), 2u);
}

TEST(SqliteHistoryStore, reopen_to_different_path_isolates_runs) {
    // Switching projects re-opens the store at a different DB path. Runs from
    // the first database must not leak into the second — the isolation
    // guarantee the desktop's per-project history relies on.
    TempDb first;
    TempDb second;
    ce::SqliteHistoryStore store;

    ASSERT_TRUE(store.open(first.path()).has_value());
    ce::RunStarted a;
    a.runId = ce::RunId{1};
    a.target = ce::OperationId{"project-a.op"};
    a.at = someTimePoint();
    ASSERT_TRUE(store.append(a).has_value());

    // Re-open at a second path (no explicit close — open() must handle it).
    ASSERT_TRUE(store.open(second.path()).has_value());
    auto rowsB = store.listRuns(10);
    ASSERT_TRUE(rowsB.has_value()) << rowsB.error().detail;
    EXPECT_TRUE(rowsB->empty()) << "second project must not see the first's runs";

    ce::RunStarted b;
    b.runId = ce::RunId{2};
    b.target = ce::OperationId{"project-b.op"};
    b.at = someTimePoint();
    ASSERT_TRUE(store.append(b).has_value());

    auto rowsAfter = store.listRuns(10);
    ASSERT_TRUE(rowsAfter.has_value());
    ASSERT_EQ(rowsAfter->size(), 1u);
    EXPECT_EQ((*rowsAfter)[0].targetOp.value, "project-b.op");

    // Re-open the first database — its single run is still there, untouched.
    ASSERT_TRUE(store.open(first.path()).has_value());
    auto rowsA = store.listRuns(10);
    ASSERT_TRUE(rowsA.has_value());
    ASSERT_EQ(rowsA->size(), 1u);
    EXPECT_EQ((*rowsA)[0].targetOp.value, "project-a.op");
}

TEST(SqliteHistoryStore, clear_deletes_all_runs_and_events) {
    TempDb tmp;
    ce::SqliteHistoryStore store;
    ASSERT_TRUE(store.open(tmp.path()).has_value());

    ce::RunStarted rs;
    rs.runId = ce::RunId{1};
    rs.target = ce::OperationId{"a.b"};
    rs.at = someTimePoint();
    ASSERT_TRUE(store.append(rs).has_value());
    ce::RunEnded re;
    re.runId = ce::RunId{1};
    re.outcome = ce::RunOutcome::Succeeded;
    re.at = someTimePoint();
    ASSERT_TRUE(store.append(re).has_value());

    ASSERT_FALSE(store.listRuns(10)->empty());

    ASSERT_TRUE(store.clear().has_value());

    auto rows = store.listRuns(10);
    ASSERT_TRUE(rows.has_value());
    EXPECT_TRUE(rows->empty());
    auto events = store.eventsFor(ce::RunId{1});
    ASSERT_TRUE(events.has_value());
    EXPECT_TRUE(events->empty());
}

// ─── Run id allocation (D-01) ───────────────────────────────────────────────

TEST(SqliteHistoryStore, allocate_run_id_is_unique_across_connections_and_reopens) {
    // Two processes (here: two connections) on one database, plus a reopen,
    // must never hand out the same run id.
    TempDb tmp;
    ce::SqliteHistoryStore a;
    ce::SqliteHistoryStore b;
    ASSERT_TRUE(a.open(tmp.path()).has_value());
    ASSERT_TRUE(b.open(tmp.path()).has_value());

    std::vector<std::uint64_t> ids;
    for (int i = 0; i < 3; ++i) {
        auto fromA = a.allocateRunId();
        ASSERT_TRUE(fromA.has_value()) << fromA.error().detail;
        ids.push_back(fromA->value);
        auto fromB = b.allocateRunId();
        ASSERT_TRUE(fromB.has_value()) << fromB.error().detail;
        ids.push_back(fromB->value);
    }
    a.close();
    ASSERT_TRUE(a.open(tmp.path()).has_value());
    auto afterReopen = a.allocateRunId();
    ASSERT_TRUE(afterReopen.has_value());
    ids.push_back(afterReopen->value);

    std::vector<std::uint64_t> sorted = ids;
    std::ranges::sort(sorted);
    EXPECT_EQ(std::ranges::adjacent_find(sorted), sorted.end()) << "duplicate run id allocated";
    for (const auto id : ids) {
        EXPECT_NE(id, 0u);
    }
}

TEST(SqliteHistoryStore, allocate_run_id_skips_ids_already_in_the_database) {
    // A database written by an older build (process-local counters) already
    // holds runs; allocation must start above them.
    TempDb tmp;
    ce::SqliteHistoryStore store;
    ASSERT_TRUE(store.open(tmp.path()).has_value());
    ce::RunStarted rs;
    rs.runId = ce::RunId{7};
    rs.target = ce::OperationId{"old.run"};
    rs.at = someTimePoint();
    ASSERT_TRUE(store.append(rs).has_value());

    auto id = store.allocateRunId();
    ASSERT_TRUE(id.has_value()) << id.error().detail;
    EXPECT_GT(id->value, 7u);
}

TEST(SqliteHistoryStore, run_started_for_existing_run_never_overwrites_it) {
    TempDb tmp;
    ce::SqliteHistoryStore store;
    ASSERT_TRUE(store.open(tmp.path()).has_value());

    ce::RunStarted original;
    original.runId = ce::RunId{1};
    original.target = ce::OperationId{"first.op"};
    original.envName = "local";
    original.chainSize = 2;
    original.at = someTimePoint();
    ASSERT_TRUE(store.append(original).has_value());
    ce::RunEnded originalEnd;
    originalEnd.runId = ce::RunId{1};
    originalEnd.outcome = ce::RunOutcome::Succeeded;
    originalEnd.at = someTimePoint() + std::chrono::seconds{1};
    ASSERT_TRUE(store.append(originalEnd).has_value());

    // A second run that (wrongly) reuses id 1: its RunStarted and every
    // later event must be rejected rather than merged into run 1.
    ce::RunStarted clash;
    clash.runId = ce::RunId{1};
    clash.target = ce::OperationId{"second.op"};
    clash.envName = "prod";
    clash.chainSize = 5;
    clash.at = someTimePoint() + std::chrono::seconds{60};
    EXPECT_FALSE(store.append(clash).has_value());
    ce::StepStarted clashStep;
    clashStep.runId = ce::RunId{1};
    clashStep.op = ce::OperationId{"second.op"};
    clashStep.at = someTimePoint() + std::chrono::seconds{61};
    EXPECT_FALSE(store.append(clashStep).has_value());
    ce::RunEnded clashEnd;
    clashEnd.runId = ce::RunId{1};
    clashEnd.outcome = ce::RunOutcome::Failed;
    clashEnd.at = someTimePoint() + std::chrono::seconds{62};
    EXPECT_FALSE(store.append(clashEnd).has_value());

    auto rows = store.listRuns(10);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 1u);
    EXPECT_EQ((*rows)[0].targetOp.value, "first.op");
    EXPECT_EQ((*rows)[0].envName, "local");
    EXPECT_EQ((*rows)[0].outcome, "Succeeded");

    auto events = store.eventsFor(ce::RunId{1});
    ASSERT_TRUE(events.has_value());
    EXPECT_EQ(events->size(), 2u) << "events of the clashing run were merged into run 1";
}

// A database from before FR-15.8 gains the initiator columns when it is opened, and a migration
// that cannot be applied is an error, not a silently half-upgraded store.
TEST(SqliteHistoryStore, a_database_without_the_initiator_columns_is_upgraded_on_open) {
    TempDb tmp;
    {
        sqlite3* rawDatabase{};
        ASSERT_EQ(sqlite3_open(tmp.path().string().c_str(), &rawDatabase), SQLITE_OK);
        std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database{rawDatabase, &sqlite3_close};
        constexpr const char* kSql = R"SQL(
            CREATE TABLE runs (run_id INTEGER PRIMARY KEY, target_op TEXT, env_name TEXT,
                               started_at TEXT, ended_at TEXT, outcome TEXT, chain_size INTEGER);
            INSERT INTO runs (run_id, target_op, started_at) VALUES
                (3, 'old.get', '2025-05-27T12:00:00Z');
        )SQL";
        ASSERT_EQ(sqlite3_exec(database.get(), kSql, nullptr, nullptr, nullptr), SQLITE_OK);
    }
    ce::SqliteHistoryStore store;
    ASSERT_TRUE(store.open(tmp.path()).has_value());
    ce::RunStarted started{ce::RunId{4}, ce::OperationId{"new.get"}, 1, "e", someTimePoint(), {}};
    started.initiator = ce::RunInitiator{ce::RunInitiator::Kind::Agent, "agent-x"};
    ASSERT_TRUE(store.append(started).has_value());
    const auto runs = store.listRuns(10);
    ASSERT_TRUE(runs.has_value());
    ASSERT_EQ(runs->size(), 2U);
    for (const auto& run : *runs) {
        if (run.runId.value == 4) {
            EXPECT_EQ(run.initiator.name, "agent-x");
        } else {
            EXPECT_EQ(run.initiator.kind, ce::RunInitiator::Kind::App);
            EXPECT_EQ(run.elapsedMs, -1);
        }
    }
    // Opening again finds every column and changes nothing.
    store.close();
    ASSERT_TRUE(store.open(tmp.path()).has_value());
}

TEST(SqliteHistoryStore, a_migration_that_cannot_be_applied_fails_the_open_and_says_why) {
    TempDb tmp;
    sqlite3* rawDatabase{};
    ASSERT_EQ(sqlite3_open(tmp.path().string().c_str(), &rawDatabase), SQLITE_OK);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database{rawDatabase, &sqlite3_close};
    constexpr const char* kSql = R"SQL(
        PRAGMA journal_mode = WAL;
        CREATE TABLE schema_version (version INTEGER PRIMARY KEY);
        INSERT INTO schema_version(version) VALUES (1);
        CREATE TABLE runs (run_id INTEGER PRIMARY KEY, target_op TEXT, env_name TEXT,
                           started_at TEXT, ended_at TEXT, outcome TEXT, chain_size INTEGER,
                           elapsed_ms INTEGER);
    )SQL";
    ASSERT_EQ(sqlite3_exec(database.get(), kSql, nullptr, nullptr, nullptr), SQLITE_OK);
    // Another process is writing for longer than the busy timeout.
    ASSERT_EQ(sqlite3_exec(database.get(), "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr),
              SQLITE_OK);
    ce::SqliteHistoryStore store;
    const auto opened = store.open(tmp.path());
    sqlite3_exec(database.get(), "ROLLBACK;", nullptr, nullptr, nullptr);
    ASSERT_FALSE(opened.has_value());
    EXPECT_NE(opened.error().detail.find("upgrade"), std::string::npos) << opened.error().detail;
}

TEST(SqliteHistoryStore, an_up_to_date_database_opens_while_another_process_writes) {
    TempDb tmp;
    {
        ce::SqliteHistoryStore creator;
        ASSERT_TRUE(creator.open(tmp.path()).has_value());
    }
    sqlite3* rawDatabase{};
    ASSERT_EQ(sqlite3_open(tmp.path().string().c_str(), &rawDatabase), SQLITE_OK);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database{rawDatabase, &sqlite3_close};
    ASSERT_EQ(sqlite3_exec(database.get(), "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr),
              SQLITE_OK);
    ce::SqliteHistoryStore store;
    const auto opened = store.open(tmp.path());
    sqlite3_exec(database.get(), "ROLLBACK;", nullptr, nullptr, nullptr);
    EXPECT_TRUE(opened.has_value()) << opened.error().detail;
}

// `reqloom show` only reads: it opens the history read-only and never upgrades or creates it.
TEST(SqliteHistoryStore, a_read_only_open_neither_creates_nor_upgrades_the_database) {
    TempDb tmp;
    {
        ce::SqliteHistoryStore store;
        EXPECT_FALSE(store.openReadOnly(tmp.path()).has_value());
        EXPECT_FALSE(fs::exists(tmp.path()));
    }
    {
        sqlite3* rawDatabase{};
        ASSERT_EQ(sqlite3_open(tmp.path().string().c_str(), &rawDatabase), SQLITE_OK);
        std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database{rawDatabase, &sqlite3_close};
        ASSERT_EQ(sqlite3_exec(database.get(),
                               "CREATE TABLE runs (run_id INTEGER PRIMARY KEY, target_op TEXT);",
                               nullptr,
                               nullptr,
                               nullptr),
                  SQLITE_OK);
    }
    ce::SqliteHistoryStore store;
    EXPECT_FALSE(store.openReadOnly(tmp.path()).has_value());
    sqlite3* rawDatabase{};
    ASSERT_EQ(sqlite3_open(tmp.path().string().c_str(), &rawDatabase), SQLITE_OK);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database{rawDatabase, &sqlite3_close};
    int tables = 0;
    sqlite3_exec(
        database.get(),
        "SELECT count(*) FROM sqlite_master WHERE type = 'table';",
        [](void* out, int, char** values, char**) {
            *static_cast<int*>(out) = std::atoi(values[0]);
            return 0;
        },
        &tables,
        nullptr);
    EXPECT_EQ(tables, 1) << "a read-only open created tables";

    // An up-to-date database reads normally.
    TempDb current;
    {
        ce::SqliteHistoryStore writer;
        ASSERT_TRUE(writer.open(current.path()).has_value());
        ASSERT_TRUE(writer
                        .append(ce::RunStarted{
                            ce::RunId{1}, ce::OperationId{"a.b"}, 1, "e", someTimePoint(), {}})
                        .has_value());
    }
    ce::SqliteHistoryStore reader;
    ASSERT_TRUE(reader.openReadOnly(current.path()).has_value());
    const auto runs = reader.listRuns(5);
    ASSERT_TRUE(runs.has_value());
    EXPECT_EQ(runs->size(), 1U);
    EXPECT_FALSE(reader
                     .append(ce::RunStarted{
                         ce::RunId{2}, ce::OperationId{"a.b"}, 1, "e", someTimePoint(), {}})
                     .has_value());
}
