// RunController run lifecycle: a cancel that races the run id, teardown during a run, and
// GUI-thread reads of a run context the worker is using.
#include "application/ProjectModel.h"
#include "application/RunController.h"

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QObject>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThreadPool>

#include <array>
#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace reqloom::desktop::tests {

namespace {

namespace ce = reqloom::engine;
namespace fs = std::filesystem;

const ce::ActorId kUser{"user"};

[[nodiscard]] ce::Operation unreachableGet(std::string name, int attempts) {
    ce::Operation op;
    op.id = ce::OperationId{"item." + name};
    op.resource = ce::ResourceId{"item"};
    op.method = ce::HttpMethod::Get;
    op.pathTemplate = "/" + name;
    op.retry.maxAttempts = attempts;
    op.retry.baseBackoff = std::chrono::milliseconds{20'000};
    op.retry.maxBackoff = std::chrono::milliseconds{20'000};
    return op;
}

/// Port 1 on loopback refuses the connection at once, so a send fails fast and retryably.
[[nodiscard]] ce::Project buildProject() {
    ce::Project p;
    p.name = "Lifecycle";
    p.defaultEnvironment = "local";
    p.environments["local"] = {{"baseUrl", "http://127.0.0.1:1"}};

    ce::Actor user;
    user.id = kUser;
    user.strategy = ce::AuthStrategy::Simple;
    ce::AuthStep step;
    step.id = "login";
    step.method = ce::HttpMethod::Post;
    step.pathTemplate = "/login";
    step.expectStatus = 200;
    user.authSteps.push_back(std::move(step));
    p.actors[user.id] = std::move(user);

    ce::Resource res;
    res.id = ce::ResourceId{"item"};
    res.operations["get"] = unreachableGet("get", 1);
    res.operations["slow"] = unreachableGet("slow", 2);
    ce::Operation authed = unreachableGet("authed", 1);
    authed.actor = kUser;
    res.operations["authed"] = std::move(authed);
    p.resources[res.id] = std::move(res);
    return p;
}

[[nodiscard]] ce::ActorSession liveSession() {
    ce::ActorSession session;
    session.state = ce::ActorSession::State::Live;
    session.expiresAt = std::chrono::steady_clock::now() + std::chrono::hours{1};
    return session;
}

class RunControllerLifecycleFixture : public ::testing::Test {
protected:
    void SetUp() override {
        if (QCoreApplication::instance() == nullptr) {
            app_ = std::make_unique<QCoreApplication>(argc_, argv_.data());
        }
        ASSERT_TRUE(dir_.isValid());
        ASSERT_TRUE(ce::writeProject(fs::path{dir_.path().toStdString()},
                                     buildProject(),
                                     /*overwrite=*/true)
                        .has_value());
        project_.loadFromDirectory(dir_.path());
        ASSERT_TRUE(project_.hasProject());
        controller_ = std::make_unique<RunController>(engine_, project_, nullptr);
        QObject::connect(controller_.get(),
                         &RunController::runFinished,
                         &receiver_,
                         [this](const RunReport& report) { report_ = report; });
    }

    void TearDown() override {
        controller_.reset();
        QThreadPool::globalInstance()->waitForDone();
    }

    /// Spin the event loop until the controller delivers `runFinished`.
    [[nodiscard]] bool waitForReport() {
        QElapsedTimer timer;
        timer.start();
        while (!report_ && timer.elapsed() < 30'000) {
            QThreadPool::globalInstance()->waitForDone(50);
            QCoreApplication::processEvents();
        }
        return report_.has_value();
    }

    int argc_{1};
    std::string argv0_{"lifecycle-test"};
    std::array<char*, 2> argv_{argv0_.data(), nullptr};
    std::unique_ptr<QCoreApplication> app_;
    QTemporaryDir dir_;
    ProjectModel project_;
    ce::ExecutionEngine engine_{ce::makeDefaultDependencies()};
    std::unique_ptr<RunController> controller_;
    QObject receiver_;
    std::optional<RunReport> report_;
};

/// Holds every pool thread so a queued run cannot start until `release()`.
class PoolBlocker {
public:
    PoolBlocker() {
        auto* pool = QThreadPool::globalInstance();
        savedMax_ = pool->maxThreadCount();
        pool->setMaxThreadCount(1);
        pool->start([gate = gate_.get_future().share()]() { gate.wait(); });
    }
    ~PoolBlocker() {
        release();
        QThreadPool::globalInstance()->waitForDone();
        QThreadPool::globalInstance()->setMaxThreadCount(savedMax_);
    }
    PoolBlocker(const PoolBlocker&) = delete;
    PoolBlocker& operator=(const PoolBlocker&) = delete;

    void release() {
        if (!released_) {
            released_ = true;
            gate_.set_value();
        }
    }

private:
    std::promise<void> gate_;
    bool released_{false};
    int savedMax_{0};
};

}  // namespace

TEST_F(RunControllerLifecycleFixture, cancel_before_the_run_id_is_known_cancels_the_run) {
    PoolBlocker blocker;
    controller_->run(QStringLiteral("item.get"), QString{}, /*clean=*/false, /*dryRun=*/false);

    controller_->cancelRun();
    blocker.release();

    ASSERT_TRUE(waitForReport());
    EXPECT_FALSE(report_->engineError);
    EXPECT_EQ(report_->outcome, ce::RunOutcome::Cancelled);
}

TEST_F(RunControllerLifecycleFixture,
       destroying_the_controller_mid_run_cancels_instead_of_waiting) {
    controller_->run(QStringLiteral("item.slow"), QString{}, /*clean=*/false, /*dryRun=*/false);

    QElapsedTimer timer;
    timer.start();
    controller_.reset();

    EXPECT_LT(timer.elapsed(), 10'000);
}

TEST_F(RunControllerLifecycleFixture,
       session_reads_during_a_run_serve_the_snapshot_taken_before_it) {
    ce::RunContext& ctx = controller_->contextForActive();
    ctx.putSession(kUser, liveSession());
    ctx.setCookie(kUser, "sid", "before-run");

    // A clean run clears the context's sessions on the worker before resolving the chain.
    controller_->run(QStringLiteral("item.authed"), QString{}, /*clean=*/true, /*dryRun=*/true);
    QThreadPool::globalInstance()->waitForDone();

    ASSERT_TRUE(controller_->isRunning());
    EXPECT_EQ(controller_->sessionInfo(kUser).state, ce::ActorSession::State::Live);
    EXPECT_GT(controller_->sessionInfo(kUser).secondsRemaining, 0);
    EXPECT_EQ(controller_->cookies(kUser).at("sid"), "before-run");
    ASSERT_EQ(controller_->jarCookies(kUser).size(), 1U);

    ASSERT_TRUE(waitForReport());
    EXPECT_EQ(controller_->sessionInfo(kUser).state, ce::ActorSession::State::None);
}

}  // namespace reqloom::desktop::tests
