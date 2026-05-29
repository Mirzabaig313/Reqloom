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

#include <chainapi/engine/Events.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

namespace ce = chainapi::engine;
namespace fs = std::filesystem;

namespace {

class TempDb {
public:
    TempDb() {
        path_ = fs::temp_directory_path() /
                ("chainapi-history-" + std::to_string(::getpid()) + "-" +
                 std::to_string(counter_++) + ".sqlite");
    }
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
    inline static int counter_{0};
};

[[nodiscard]] ce::TimePoint someTimePoint() {
    // Fixed instant — round-tripping via ISO-8601 truncates sub-second
    // precision, so use a value that has none to start with.
    using namespace std::chrono;
    return system_clock::time_point{seconds{1748352000}};  // 2025-05-27T12:00:00Z
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
    const auto root = fs::temp_directory_path() /
                      ("chainapi-history-mkdir-" + std::to_string(::getpid()));
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

TEST(SqliteHistoryStore, step_failed_event_round_trips_with_error_code) {
    TempDb tmp;
    ce::SqliteHistoryStore store;
    ASSERT_TRUE(store.open(tmp.path()).has_value());

    ce::RunStarted rs;
    rs.runId = ce::RunId{9};
    rs.target = ce::OperationId{"x.y"};
    rs.at = someTimePoint();
    store.append(rs);

    ce::StepFailed sf;
    sf.runId = ce::RunId{9};
    sf.stepIndex = 1;
    sf.op = ce::OperationId{"x.y"};
    sf.code = ce::ErrorCode::ExtractionFailed;
    sf.cls = ce::classify(ce::ErrorCode::ExtractionFailed);
    sf.attempt = 3;
    sf.detail = "extract 'token' missed";
    sf.at = someTimePoint();
    ASSERT_TRUE(store.append(sf).has_value());

    auto replayed = store.eventsFor(ce::RunId{9});
    ASSERT_TRUE(replayed.has_value());
    ASSERT_EQ(replayed->size(), 2u);

    const auto* sfBack = std::get_if<ce::StepFailed>(&(*replayed)[1]);
    ASSERT_NE(sfBack, nullptr);
    EXPECT_EQ(sfBack->code, ce::ErrorCode::ExtractionFailed);
    EXPECT_EQ(sfBack->cls, ce::ErrorClass::Extraction);
    EXPECT_EQ(sfBack->attempt, 3);
    EXPECT_EQ(sfBack->detail, "extract 'token' missed");
}

TEST(SqliteHistoryStore, step_cancelled_and_session_refreshed_round_trip) {
    TempDb tmp;
    ce::SqliteHistoryStore store;
    ASSERT_TRUE(store.open(tmp.path()).has_value());

    ce::RunStarted rs;
    rs.runId = ce::RunId{11};
    rs.target = ce::OperationId{"x.y"};
    rs.at = someTimePoint();
    store.append(rs);

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
    ASSERT_EQ(replayed->size(), 3u);

    const auto* scBack = std::get_if<ce::StepCancelled>(&(*replayed)[1]);
    ASSERT_NE(scBack, nullptr);
    EXPECT_EQ(scBack->op.value, "first.get");

    const auto* srBack = std::get_if<ce::SessionRefreshed>(&(*replayed)[2]);
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
