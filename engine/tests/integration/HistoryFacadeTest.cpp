// HistoryFacadeTest — integration coverage for the public run-history read
// facade on ExecutionEngine (openHistory / listRuns / historyEvents).
//
// The store is wired into the default dependencies but inert until
// openHistory() is called. These tests exercise the full path the desktop
// uses: open a DB, run a chain against the mock SUT, then read the run back
// out through the public API without touching infrastructure headers.
#include "MockSutHarness.h"

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <support/TempPath.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <variant>

namespace ce = reqloom::engine;
namespace ct = reqloom::tests;
namespace fs = std::filesystem;

namespace {

[[nodiscard]] fs::path fixturesDir() {
    return fs::path(REQLOOM_FIXTURES_DIR);
}

/// Scratch project on disk; cleaned up on destruction.
class ScratchProject {
public:
    explicit ScratchProject(const std::string& yamlBody) {
        path_ = ct::uniqueTempPath("reqloom-history-facade");
        fs::create_directories(path_);
        std::ofstream{path_ / "reqloom.yaml"} << yamlBody;
    }

    ~ScratchProject() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    ScratchProject(const ScratchProject&) = delete;
    ScratchProject& operator=(const ScratchProject&) = delete;

    [[nodiscard]] fs::path yaml() const { return path_ / "reqloom.yaml"; }

private:
    fs::path path_;
};

constexpr const char* kProjectYaml = R"YAML(
version: 1
name: HistoryFacadeTest
default_environment: local

environment:
  baseUrl: http://placeholder

actors:
  user:
    auth:
      method: POST
      path: /api/v1/auth/login
      body: { email: "u@example.test" }
      extract: { token: $.data.accessToken }
    inject:
      headers: { Authorization: "Bearer {{user.token}}" }

resources:
  ping:
    operations:
      get:
        method: GET
        path: /api/v1/with-bearer
        actor: user
        expect_status: 200
        extract:
          ping_id: $.id
)YAML";

/// Temp history DB whose sidecar WAL/SHM files are also cleaned up.
class TempDb {
public:
    TempDb() : path_(ct::uniqueTempPath("reqloom-history-facade", ".sqlite")) {}
    ~TempDb() {
        std::error_code ec;
        fs::remove(path_, ec);
        fs::remove(fs::path{path_.string() + "-wal"}, ec);
        fs::remove(fs::path{path_.string() + "-shm"}, ec);
    }
    TempDb(const TempDb&) = delete;
    TempDb& operator=(const TempDb&) = delete;

    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

class HistoryFacadeFixture : public ::testing::Test {
protected:
    void SetUp() override {
        harness_ = std::make_unique<ct::MockSutHarness>(fixturesDir() / "polling-routes.json");
    }
    void TearDown() override { harness_.reset(); }

    std::unique_ptr<ct::MockSutHarness> harness_;
};

}  // namespace

TEST_F(HistoryFacadeFixture, list_runs_returns_persisted_run_after_open) {
    ScratchProject project(kProjectYaml);
    auto loaded = ce::parseProject(project.yaml());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().detail;
    loaded->environments["local"]["baseUrl"] = harness_->baseUrl();

    TempDb db;
    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    auto opened = engine.openHistory(db.path());
    ASSERT_TRUE(opened.has_value()) << opened.error().detail;

    ce::RunContext ctx;
    auto run = engine.run(*loaded, ce::OperationId{"ping.get"}, ctx);
    ASSERT_TRUE(run.has_value()) << run.error().detail;
    EXPECT_TRUE(run->succeeded());

    auto runs = engine.listRuns(10);
    ASSERT_TRUE(runs.has_value()) << runs.error().detail;
    ASSERT_EQ(runs->size(), 1u);

    const auto& row = runs->front();
    EXPECT_EQ(row.target.value, "ping.get");
    EXPECT_EQ(row.envName, "local");
    EXPECT_EQ(row.outcome, "Succeeded");
    EXPECT_FALSE(row.startedAt.empty());
    EXPECT_FALSE(row.endedAt.empty());
    EXPECT_EQ(row.runId.value, run->runId.value);
    // Duration comes from the RunEnded event's real elapsed (a steady_clock
    // delta around run()), not a hardcoded 0 or a second-resolution timestamp
    // diff. A real loopback chain with an auth round-trip always takes >1ms.
    EXPECT_GT(row.elapsedMs, 0);
}

TEST_F(HistoryFacadeFixture, history_events_replays_the_run_timeline) {
    ScratchProject project(kProjectYaml);
    auto loaded = ce::parseProject(project.yaml());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().detail;
    loaded->environments["local"]["baseUrl"] = harness_->baseUrl();

    TempDb db;
    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ASSERT_TRUE(engine.openHistory(db.path()).has_value());

    ce::RunContext ctx;
    auto run = engine.run(*loaded, ce::OperationId{"ping.get"}, ctx);
    ASSERT_TRUE(run.has_value()) << run.error().detail;

    auto events = engine.historyEvents(run->runId);
    ASSERT_TRUE(events.has_value()) << events.error().detail;
    ASSERT_FALSE(events->empty());

    EXPECT_TRUE(std::holds_alternative<ce::RunStarted>(events->front()));
    EXPECT_TRUE(std::holds_alternative<ce::RunEnded>(events->back()));
}

TEST_F(HistoryFacadeFixture, list_runs_without_open_reports_internal_error) {
    // No openHistory() call — the store is inert, so the read facade must
    // surface a clean error rather than crash or silently return nothing.
    ce::ExecutionEngine engine(ce::makeDefaultDependencies());

    auto runs = engine.listRuns(10);
    ASSERT_FALSE(runs.has_value());
}
