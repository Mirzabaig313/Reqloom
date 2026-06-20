// ForEachTest — end-to-end HTTP fan-out against the mock SUT. A list endpoint
// extracts N ids with a `[*]` path; a `for_each` detail endpoint then runs once
// per id, fetching /api/v1/orgs/<id>. Confirms the fan-out actually issues one
// HTTP request per item, and that the continue-on-error flag controls whether a
// failed iteration stops the remaining ones.
#include "MockSutHarness.h"

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <support/TempPath.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
namespace ce = reqloom::engine;
namespace ct = reqloom::tests;

namespace {

[[nodiscard]] fs::path fixturesDir() {
    return fs::path(REQLOOM_FIXTURES_DIR);
}

/// A throwaway project directory holding a single reqloom.yaml. Removed on
/// destruction.
class ScratchProject {
public:
    explicit ScratchProject(const std::string& yamlBody) {
        path_ = ct::uniqueTempPath("reqloom-foreach-itest");
        fs::create_directories(path_);
        std::ofstream{path_ / "reqloom.yaml"} << yamlBody;
    }

    ScratchProject(const ScratchProject&) = delete;
    ScratchProject& operator=(const ScratchProject&) = delete;

    ~ScratchProject() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    [[nodiscard]] fs::path yaml() const { return path_ / "reqloom.yaml"; }

private:
    fs::path path_;
};

/// `continueOnError` toggles `continue_on_error` on the for_each block.
[[nodiscard]] std::string projectYaml(bool continueOnError) {
    std::string forEachBlock = "        for_each:\n          over: org\n";
    if (continueOnError) {
        forEachBlock += "          continue_on_error: true\n";
    }
    return std::string{R"YAML(
version: 1
name: ForEachIntegration
default_environment: local

environment:
  baseUrl: http://placeholder

resources:
  org:
    operations:
      list:
        method: GET
        path: /api/v1/orgs
        expect_status: 200
        extract:
          org_id: $.data.items[*].id
  org_detail:
    operations:
      get:
        method: GET
        path: /api/v1/orgs/{{org.org_id}}
        expect_status: 200
)YAML"} + forEachBlock +
           R"YAML(        extract:
          org_name: $.name
)YAML";
}

/// Number of iteration rows for the for_each target (rows carrying a
/// forEachIndex). The parent summary row leaves forEachIndex empty.
[[nodiscard]] std::size_t iterationCount(const ce::RunResult& result, const std::string& opId) {
    std::size_t count = 0;
    for (const auto& step : result.steps) {
        if (step.op.value == opId && step.forEachIndex.has_value()) {
            ++count;
        }
    }
    return count;
}

}  // namespace

class ForEachFanOut : public ::testing::Test {
protected:
    [[nodiscard]] ce::Project load(const std::string& yamlBody) {
        ScratchProject project(yamlBody);
        auto loaded = ce::parseProject(project.yaml());
        EXPECT_TRUE(loaded.has_value()) << (loaded ? "" : loaded.error().detail);
        loaded->environments["local"]["baseUrl"] = harness_->baseUrl();
        return std::move(*loaded);
    }

    std::unique_ptr<ct::MockSutHarness> harness_;
};

TEST_F(ForEachFanOut, runs_detail_once_per_list_item_when_all_succeed) {
    harness_ = std::make_unique<ct::MockSutHarness>(fixturesDir() / "foreach-routes.json");
    auto project = load(projectYaml(/*continueOnError=*/false));

    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;
    auto result = engine.run(project, ce::OperationId{"org_detail.get"}, ctx);
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().detail);

    EXPECT_TRUE(result->succeeded());
    EXPECT_EQ(iterationCount(*result, "org_detail.get"), 3u) << "one HTTP request per list item";
}

TEST_F(ForEachFanOut, stops_at_first_failed_iteration_by_default) {
    harness_ = std::make_unique<ct::MockSutHarness>(fixturesDir() / "foreach-fail-routes.json");
    auto project = load(projectYaml(/*continueOnError=*/false));

    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;
    auto result = engine.run(project, ce::OperationId{"org_detail.get"}, ctx);
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().detail);

    EXPECT_FALSE(result->succeeded());
    // org-1 succeeds, org-2 fails and halts the fan-out — org-3 never runs.
    EXPECT_EQ(iterationCount(*result, "org_detail.get"), 2u);
}

TEST_F(ForEachFanOut, continue_on_error_runs_every_iteration_despite_a_failure) {
    harness_ = std::make_unique<ct::MockSutHarness>(fixturesDir() / "foreach-fail-routes.json");
    auto project = load(projectYaml(/*continueOnError=*/true));

    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;
    auto result = engine.run(project, ce::OperationId{"org_detail.get"}, ctx);
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().detail);

    // The run still fails overall (org-2 errored), but all three items ran.
    EXPECT_FALSE(result->succeeded());
    EXPECT_EQ(iterationCount(*result, "org_detail.get"), 3u);
}
