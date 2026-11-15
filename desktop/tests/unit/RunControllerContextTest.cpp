// RunController run-context isolation: one context per (project root,
// environment name), so sessions, cookies and cached extractions obtained in
// one environment are never used in another. Desktop Requirement FR-5.1.
#include "application/ProjectModel.h"
#include "application/RunController.h"

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <QtCore/QTemporaryDir>
#include <QtCore/QThreadPool>

#include <chrono>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace reqloom::desktop::tests {

namespace {

namespace ce = reqloom::engine;
namespace fs = std::filesystem;

const ce::ActorId kUser{"user"};
const ce::ResourceId kPayment{"payment"};
const QString kStaging{QStringLiteral("staging")};
const QString kProduction{QStringLiteral("production")};

/// Two environments (staging is the default), one actor, one operation.
[[nodiscard]] ce::Project buildProject() {
    ce::Project p;
    p.name = "Envs";
    p.defaultEnvironment = "staging";
    p.environments["staging"] = {{"baseUrl", "http://staging.invalid"}};
    p.environments["production"] = {{"baseUrl", "http://production.invalid"}};

    ce::Actor user;
    user.id = kUser;
    user.strategy = ce::AuthStrategy::Simple;
    ce::AuthStep step;
    step.id = "login";
    step.method = ce::HttpMethod::Post;
    step.pathTemplate = "/login";
    step.expectStatus = 200;
    step.extractions.push_back({"token", "$.token", ce::Extraction::Source::JsonPath});
    user.authSteps.push_back(std::move(step));
    user.inject.headers["Authorization"] = "Bearer {{user.token}}";
    p.actors[user.id] = std::move(user);

    ce::Resource res;
    res.id = kPayment;
    ce::Operation op;
    op.id = ce::OperationId{"payment.pay"};
    op.resource = res.id;
    op.actor = kUser;
    op.method = ce::HttpMethod::Get;
    op.pathTemplate = "/pay";
    res.operations["pay"] = std::move(op);
    p.resources[res.id] = std::move(res);
    return p;
}

[[nodiscard]] ce::ActorSession liveSession() {
    ce::ActorSession session;
    session.state = ce::ActorSession::State::Live;
    session.injectHeaders["Authorization"] = "Bearer staging-token";
    session.expiresAt = std::chrono::steady_clock::now() + std::chrono::hours{1};
    return session;
}

[[nodiscard]] ce::ResourceInstance instanceWithId(std::string id) {
    ce::ResourceInstance instance;
    instance.variables["id"] = std::move(id);
    return instance;
}

class RunControllerContextFixture : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(dir_.isValid());
        const fs::path root{dir_.path().toStdString()};
        ASSERT_TRUE(ce::writeProject(root, buildProject(), /*overwrite=*/true).has_value());
        project_.loadFromDirectory(dir_.path());
        ASSERT_TRUE(project_.hasProject());
        controller_ = std::make_unique<RunController>(engine_, project_, nullptr);
    }

    void TearDown() override {
        controller_.reset();
        QThreadPool::globalInstance()->waitForDone();
    }

    /// Seed a live session, a cookie and one cached instance into the active
    /// environment's context.
    void seedActiveContext(const std::string& id) {
        ce::RunContext& ctx = controller_->contextForActive();
        ctx.putSession(kUser, liveSession());
        ctx.setCookie(kUser, "sid", "cookie-" + id);
        ctx.appendInstance(kPayment, instanceWithId(id));
    }

    [[nodiscard]] std::size_t activeInstanceCount() const {
        const ce::RunContext* ctx = controller_->findActiveContext();
        return ctx == nullptr ? 0U : ctx->instances(kPayment).size();
    }

    QTemporaryDir dir_;
    ProjectModel project_;
    ce::ExecutionEngine engine_{ce::makeDefaultDependencies()};
    std::unique_ptr<RunController> controller_;
};

}  // namespace

TEST_F(RunControllerContextFixture, session_from_one_environment_is_not_visible_in_another) {
    controller_->setEnvironment(kStaging);
    seedActiveContext("stg-1");

    controller_->setEnvironment(kProduction);

    EXPECT_EQ(controller_->sessionInfo(kUser).state, ce::ActorSession::State::None);
    EXPECT_TRUE(controller_->cookies(kUser).empty());
    EXPECT_EQ(activeInstanceCount(), 0U);
}

TEST_F(RunControllerContextFixture, switching_back_finds_the_earlier_environment_state) {
    controller_->setEnvironment(kStaging);
    seedActiveContext("stg-1");
    controller_->setEnvironment(kProduction);
    seedActiveContext("prd-1");

    controller_->setEnvironment(kStaging);

    EXPECT_EQ(controller_->sessionInfo(kUser).state, ce::ActorSession::State::Live);
    EXPECT_EQ(controller_->cookies(kUser).at("sid"), "cookie-stg-1");
    ASSERT_EQ(activeInstanceCount(), 1U);
    EXPECT_EQ(controller_->findActiveContext()->instances(kPayment).front().variables.at("id"),
              "stg-1");
}

TEST_F(RunControllerContextFixture, empty_environment_shares_the_default_environment_context) {
    controller_->setEnvironment(QString{});
    seedActiveContext("default-1");

    controller_->setEnvironment(kStaging);

    EXPECT_EQ(controller_->sessionInfo(kUser).state, ce::ActorSession::State::Live);
}

TEST_F(RunControllerContextFixture, reset_in_one_environment_leaves_the_other_untouched) {
    controller_->setEnvironment(kStaging);
    seedActiveContext("stg-1");
    controller_->setEnvironment(kProduction);
    seedActiveContext("prd-1");

    controller_->resetCaches();

    EXPECT_EQ(controller_->sessionInfo(kUser).state, ce::ActorSession::State::None);
    EXPECT_TRUE(controller_->cookies(kUser).empty());
    EXPECT_EQ(activeInstanceCount(), 0U);
    controller_->setEnvironment(kStaging);
    EXPECT_EQ(controller_->sessionInfo(kUser).state, ce::ActorSession::State::Live);
    EXPECT_EQ(activeInstanceCount(), 1U);
}

TEST_F(RunControllerContextFixture, clear_extraction_cache_only_touches_the_active_environment) {
    controller_->setEnvironment(kStaging);
    seedActiveContext("stg-1");
    controller_->setEnvironment(kProduction);
    seedActiveContext("prd-1");

    controller_->clearExtractionCache();

    EXPECT_EQ(activeInstanceCount(), 0U);
    EXPECT_EQ(controller_->sessionInfo(kUser).state, ce::ActorSession::State::Live);
    controller_->setEnvironment(kStaging);
    EXPECT_EQ(activeInstanceCount(), 1U);
}

TEST_F(RunControllerContextFixture, reset_clears_inline_oauth2_sessions) {
    const ce::ActorId inlineId{"__inline_oauth2:client_credentials|{{env.tokenUrl}}|cid||"};
    controller_->setEnvironment(kStaging);
    controller_->contextForActive().putSession(inlineId, liveSession());

    controller_->resetCaches();

    EXPECT_EQ(controller_->findActiveContext()->session(inlineId), nullptr);
}

TEST_F(RunControllerContextFixture, forgetting_the_project_drops_every_environment_context) {
    controller_->setEnvironment(kStaging);
    seedActiveContext("stg-1");
    controller_->setEnvironment(kProduction);
    seedActiveContext("prd-1");

    controller_->forgetProject(project_.rootPath());

    EXPECT_EQ(controller_->findActiveContext(), nullptr);
    controller_->setEnvironment(kStaging);
    EXPECT_EQ(controller_->findActiveContext(), nullptr);
}

TEST_F(RunControllerContextFixture, run_uses_the_context_of_the_environment_it_runs_against) {
    controller_->setEnvironment(kStaging);
    controller_->setVariableOverrides({{"payment.id", "pinned-42"}});

    controller_->run(QStringLiteral("payment.pay"), kProduction, /*clean=*/false, /*dryRun=*/true);
    QThreadPool::globalInstance()->waitForDone();

    EXPECT_EQ(activeInstanceCount(), 1U);
    controller_->setEnvironment(kStaging);
    EXPECT_EQ(controller_->findActiveContext(), nullptr);
}

}  // namespace reqloom::desktop::tests
