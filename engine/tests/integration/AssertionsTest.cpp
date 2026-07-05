// AssertionsTest — end-to-end response assertions against the mock SUT. An
// operation declares `assert:` predicates; the engine evaluates them after the
// response and records pass/fail on the StepResult, failing the step (and run)
// when any assertion is false.
#include "MockSutHarness.h"

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <support/TempPath.h>

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

class ScratchProject {
public:
    explicit ScratchProject(const std::string& yamlBody) {
        path_ = ct::uniqueTempPath("reqloom-assert-itest");
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

[[nodiscard]] std::string projectYaml(const std::string& assertBlock) {
    return std::string{R"YAML(
version: 1
name: AssertIntegration
default_environment: local

environment:
  baseUrl: http://placeholder

resources:
  order:
    operations:
      get:
        method: GET
        path: /api/v1/orders/1
        expect_status: 200
)YAML"} + assertBlock;
}

[[nodiscard]] const ce::StepResult* findStep(const ce::RunResult& r, const std::string& op) {
    for (const auto& s : r.steps) {
        if (s.op.value == op && !s.forEachIndex && !s.pollAttempt) {
            return &s;
        }
    }
    return nullptr;
}

}  // namespace

class Assertions : public ::testing::Test {
protected:
    [[nodiscard]] ce::Project load(const std::string& yamlBody) {
        ScratchProject project(yamlBody);
        auto loaded = ce::parseProject(project.yaml());
        EXPECT_TRUE(loaded.has_value()) << (loaded ? "" : loaded.error().detail);
        loaded->environments["local"]["baseUrl"] = harness_->baseUrl();
        return std::move(*loaded);
    }

    void SetUp() override {
        harness_ = std::make_unique<ct::MockSutHarness>(fixturesDir() / "assert-routes.json");
    }
    void TearDown() override { harness_.reset(); }

    std::unique_ptr<ct::MockSutHarness> harness_;
};

TEST_F(Assertions, passing_assertions_succeed_and_are_recorded) {
    auto project =
        load(projectYaml("        assert:\n"
                         "          - $.status_code == 200\n"
                         "          - $.data.status == 'confirmed'\n"
                         "          - $.data.total > 10\n"));
    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;
    auto result = engine.run(project, ce::OperationId{"order.get"}, ctx);
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().detail);
    EXPECT_TRUE(result->succeeded());

    const ce::StepResult* step = findStep(*result, "order.get");
    ASSERT_NE(step, nullptr);
    ASSERT_EQ(step->assertions.size(), 3u);
    for (const auto& a : step->assertions) {
        EXPECT_TRUE(a.passed) << a.expr;
    }
}

TEST_F(Assertions, a_failing_assertion_fails_the_step) {
    auto project =
        load(projectYaml("        assert:\n"
                         "          - $.data.status == 'shipped'\n"));
    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;
    auto result = engine.run(project, ce::OperationId{"order.get"}, ctx);
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().detail);
    EXPECT_FALSE(result->succeeded());

    const ce::StepResult* step = findStep(*result, "order.get");
    ASSERT_NE(step, nullptr);
    EXPECT_EQ(step->status, ce::StepResult::Status::Failed);
    ASSERT_EQ(step->assertions.size(), 1u);
    EXPECT_FALSE(step->assertions[0].passed);
    ASSERT_TRUE(step->error.has_value());
    EXPECT_EQ(*step->error, ce::ErrorCode::AssertionFailed);
}

TEST_F(Assertions, records_each_result_even_when_a_later_one_fails) {
    auto project =
        load(projectYaml("        assert:\n"
                         "          - $.data.id\n"
                         "          - $.data.total > 1000\n"));
    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;
    auto result = engine.run(project, ce::OperationId{"order.get"}, ctx);
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().detail);

    const ce::StepResult* step = findStep(*result, "order.get");
    ASSERT_NE(step, nullptr);
    ASSERT_EQ(step->assertions.size(), 2u);
    EXPECT_TRUE(step->assertions[0].passed);   // $.data.id is truthy
    EXPECT_FALSE(step->assertions[1].passed);  // total > 1000 is false
}
