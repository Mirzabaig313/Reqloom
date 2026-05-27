// ExtractionSourcesTest — confirms the executor's full extraction
// pipeline now resolves Header / StatusCode / Cookie sources against
// the live response, in addition to the long-standing JsonPath path.
//
// This test fails on the parent commit because:
//   - extractFromJsonDetailed treated every non-JsonPath source as
//     Unsupported, leaving the executor's RunContext extractionTrace
//     showing those vars as never resolved.
//   - The executor passed only `body` to the extractor, not headers
//     or status — even after this slice's helper changes, the wiring
//     into ExecutionEngine had to be threaded through.

#include "MockSutHarness.h"

#include <chainapi/engine/Factories.h>
#include <chainapi/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

namespace fs = std::filesystem;
namespace ce = chainapi::engine;
namespace ct = chainapi::tests;

namespace {

[[nodiscard]] fs::path fixturesDir() {
    return fs::path(CHAINAPI_FIXTURES_DIR);
}

[[nodiscard]] ce::Project loadProject(const std::string& mockBaseUrl) {
    auto project = ce::parseProject(fixturesDir() / "extraction-project" / "chainapi.yaml");
    EXPECT_TRUE(project.has_value()) << (project ? "" : project.error().detail);
    project->environments["local"]["baseUrl"] = mockBaseUrl;
    return std::move(*project);
}

}  // namespace

class ExtractionSourcesFixture : public ::testing::Test {
protected:
    void SetUp() override {
        harness_ = std::make_unique<ct::MockSutHarness>(fixturesDir() / "extraction-routes.json");
    }
    void TearDown() override { harness_.reset(); }

    std::unique_ptr<ct::MockSutHarness> harness_;
};

TEST_F(ExtractionSourcesFixture, executor_resolves_header_status_and_cookie_sources) {
    auto project = loadProject(harness_->baseUrl());
    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;

    auto result = engine.run(project, ce::OperationId{"order.create"}, ctx);
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().detail);
    ASSERT_TRUE(result->succeeded()) << "create chain did not succeed";

    // Walk the trace and confirm every declared extraction resolved
    // with the value returned by the mock SUT.
    std::map<std::string, std::string> resolved;
    for (const auto& trace : ctx.extractionTrace()) {
        if (trace.op.value != "order.create") continue;
        EXPECT_EQ(trace.outcome, ce::ExtractionTrace::Outcome::Resolved)
            << "var '" << trace.variableName << "' (path " << trace.sourcePath
            << ") did not resolve; outcome was numeric " << static_cast<int>(trace.outcome);
        resolved[trace.variableName] = trace.value;
    }

    EXPECT_EQ(resolved["id"], "ord-42");
    EXPECT_EQ(resolved["location"], "/api/v1/orders/ord-42");
    EXPECT_EQ(resolved["request_id"], "req-99");
    EXPECT_EQ(resolved["status"], "201");
    EXPECT_EQ(resolved["audit_cookie"], "trace-001");
}

TEST_F(ExtractionSourcesFixture, extracted_header_value_flows_into_subsequent_template) {
    // The whole point of resolving headers at runtime: a follow-up
    // operation can reference {{order.location}} just like any
    // body-extracted variable. The variable resolver doesn't care
    // about the source kind — it just looks at the resource cache.
    auto project = loadProject(harness_->baseUrl());
    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;

    auto result = engine.run(project, ce::OperationId{"order.create"}, ctx);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded());

    const auto& instances = ctx.instances(ce::ResourceId{"order"});
    ASSERT_FALSE(instances.empty())
        << "order.create extractions did not produce a ResourceInstance";
    const auto& latest = instances.back();
    ASSERT_TRUE(latest.variables.count("location") > 0)
        << "header-extracted `location` did not land in the resource cache";
    EXPECT_EQ(latest.variables.at("location"), "/api/v1/orders/ord-42");
}
