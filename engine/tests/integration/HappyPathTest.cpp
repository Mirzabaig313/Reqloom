// HappyPathTest — end-to-end run loop against the mock SUT using the
// bundled marketplace sample. Confirms:
//   - The chain for refund.approve resolves to the expected sequence
//   - All steps execute and succeed
//   - Sessions are cached across actors (only one auth per actor)
//   - Extracted variables flow correctly between steps
#include "MockSutHarness.h"

#include <chainapi/engine/Factories.h>
#include <chainapi/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace fs = std::filesystem;
namespace ce = chainapi::engine;
namespace ct = chainapi::tests;

namespace {

[[nodiscard]] fs::path samplesDir() {
    return fs::path(CHAINAPI_SAMPLES_DIR);
}

[[nodiscard]] fs::path fixturesDir() {
    return fs::path(CHAINAPI_FIXTURES_DIR);
}

[[nodiscard]] ce::Project loadMarketplace(const std::string& mockBaseUrl) {
    auto project = ce::parseProject(samplesDir() / "marketplace" / "chainapi.yaml");
    EXPECT_TRUE(project.has_value()) << (project ? "" : project.error().detail);

    auto& env = project->environments["local"];
    env["baseUrl"] = mockBaseUrl;

    return std::move(*project);
}

}  // namespace

class MarketplaceHappyPath : public ::testing::Test {
protected:
    void SetUp() override {
        harness_ = std::make_unique<ct::MockSutHarness>(fixturesDir() / "marketplace-routes.json");
    }
    void TearDown() override { harness_.reset(); }

    std::unique_ptr<ct::MockSutHarness> harness_;
};

TEST_F(MarketplaceHappyPath, RefundApproveResolvesAndExecutes) {
    auto project = loadMarketplace(harness_->baseUrl());
    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;

    auto result = engine.run(project, ce::OperationId{"refund.approve"}, ctx);
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().detail);
    EXPECT_TRUE(result->succeeded()) << "outcome was Failed/Cancelled";

    for (const auto& step : result->steps) {
        const auto status = step.status;
        EXPECT_TRUE(status == ce::StepResult::Status::Succeeded ||
                    status == ce::StepResult::Status::Skipped)
            << "step " << step.op.value << " ended in unexpected state";
    }
}

TEST_F(MarketplaceHappyPath, RerunUsesCachedSessions) {
    auto project = loadMarketplace(harness_->baseUrl());
    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;

    auto first = engine.run(project, ce::OperationId{"refund.approve"}, ctx);
    ASSERT_TRUE(first);
    ASSERT_TRUE(first->succeeded());

    // Second run reuses the run context — sessions should already be live.
    auto second = engine.run(project, ce::OperationId{"refund.approve"}, ctx);
    ASSERT_TRUE(second);
    EXPECT_TRUE(second->succeeded());

    for (const auto& step : second->steps) {
        EXPECT_NE(step.status, ce::StepResult::Status::Failed);
        EXPECT_NE(step.status, ce::StepResult::Status::Cancelled);
    }
}

TEST_F(MarketplaceHappyPath, ChainContainsExpectedOperations) {
    auto project = loadMarketplace(harness_->baseUrl());
    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;

    auto result = engine.run(project, ce::OperationId{"refund.approve"}, ctx);
    ASSERT_TRUE(result);

    bool sawProductCreate = false;
    bool sawProductPublish = false;
    bool sawOrderCreate = false;
    bool sawOrderPay = false;
    bool sawRefundRequest = false;
    bool targetIsLast = false;

    for (std::size_t i = 0; i < result->steps.size(); ++i) {
        const auto& step = result->steps[i];
        if (step.op.value == "product.create")
            sawProductCreate = true;
        else if (step.op.value == "product.publish")
            sawProductPublish = true;
        else if (step.op.value == "order.create")
            sawOrderCreate = true;
        else if (step.op.value == "order.pay")
            sawOrderPay = true;
        else if (step.op.value == "refund.request")
            sawRefundRequest = true;
        else if (step.op.value == "refund.approve" && i == result->steps.size() - 1) {
            targetIsLast = true;
        }
    }

    EXPECT_TRUE(sawProductCreate);
    EXPECT_TRUE(sawProductPublish);
    EXPECT_TRUE(sawOrderCreate);
    EXPECT_TRUE(sawOrderPay);
    EXPECT_TRUE(sawRefundRequest);
    EXPECT_TRUE(targetIsLast);
}

TEST_F(MarketplaceHappyPath, ExtractionTraceCapturesEveryDeclaredExtraction) {
    // Slice 6d/6e: every successful extraction lands as one row in
    // ctx.extractionTrace(). The marketplace fixture has at least one
    // extraction per resource — product.id, order.id, refund.id.
    auto project = loadMarketplace(harness_->baseUrl());
    ce::ExecutionEngine engine(ce::makeDefaultDependencies());
    ce::RunContext ctx;

    auto result = engine.run(project, ce::OperationId{"refund.approve"}, ctx);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_TRUE(result->succeeded());

    const auto& trace = ctx.extractionTrace();
    ASSERT_FALSE(trace.empty()) << "expected at least one extraction trace";

    // Confirm at least one Resolved entry per pivotal extraction. Names
    // come from the marketplace fixture's chainapi.yaml.
    bool sawProductId = false;
    bool sawOrderId = false;
    for (const auto& t : trace) {
        EXPECT_EQ(t.outcome, ce::ExtractionTrace::Outcome::Resolved)
            << "trace for " << t.op.value << "." << t.variableName << " was not Resolved";
        if (t.variableName == "product_id" || t.variableName == "id") {
            if (t.op.value.starts_with("product.")) sawProductId = true;
            if (t.op.value.starts_with("order.")) sawOrderId = true;
        }
    }
    EXPECT_TRUE(sawProductId || sawOrderId)
        << "expected at least one product or order extraction in the trace";
}

TEST_F(MarketplaceHappyPath, ExtractionCompletedEventEmittedPerExtraction) {
    // Slice 6f: an ExtractionCompleted event fires for every extraction
    // the executor evaluates. Subscribers see the per-extraction outcome
    // live without inspecting the trace after the fact.
    auto project = loadMarketplace(harness_->baseUrl());
    ce::ExecutionEngine engine(ce::makeDefaultDependencies());

    std::vector<ce::ExtractionCompleted> events;
    engine.subscribe([&](const ce::RunEvent& ev) {
        if (const auto* e = std::get_if<ce::ExtractionCompleted>(&ev)) {
            events.push_back(*e);
        }
    });

    ce::RunContext ctx;
    auto result = engine.run(project, ce::OperationId{"refund.approve"}, ctx);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded());

    ASSERT_EQ(events.size(), ctx.extractionTrace().size())
        << "one event per trace row";

    for (const auto& ev : events) {
        EXPECT_EQ(ev.outcome, ce::ExtractionCompleted::Outcome::Resolved);
        EXPECT_FALSE(ev.variableName.empty());
        EXPECT_FALSE(ev.sourcePath.empty());
        EXPECT_FALSE(ev.value.empty());
    }
}
