// HostApprovalBrokerTest — the desktop app asks before a run first sends credentials to a host
// (FR-15.9): the worker waits for the person's answer, a cancelled run releases it with a no, and
// the GUI thread itself is never blocked.
#include "application/HostApprovalBroker.h"
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
#include <QtCore/QUrl>

#include <array>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace reqloom::desktop::tests {

namespace {

namespace ce = reqloom::engine;
namespace fs = std::filesystem;

class AppFixture : public ::testing::Test {
protected:
    void SetUp() override {
        if (QCoreApplication::instance() == nullptr) {
            app_ = std::make_unique<QCoreApplication>(argc_, argv_.data());
        }
    }

    /// Spin the event loop until `done` or ten seconds pass.
    template <typename Pred>
    [[nodiscard]] static bool spinUntil(Pred done) {
        QElapsedTimer timer;
        timer.start();
        while (!done() && timer.elapsed() < 10'000) {
            QThreadPool::globalInstance()->waitForDone(20);
            QCoreApplication::processEvents();
        }
        return done();
    }

    int argc_{1};
    std::string argv0_{"host-approval-test"};
    std::array<char*, 2> argv_{argv0_.data(), nullptr};
    std::unique_ptr<QCoreApplication> app_;
};

[[nodiscard]] ce::HostApprovalRequest question() {
    return {"Shop", "shop-0001", "api.example.com:443", "https://api.example.com/x", "a secret"};
}

}  // namespace

TEST_F(AppFixture, worker_waits_for_the_answer_given_on_the_gui_thread) {
    HostApprovalBroker broker;
    std::optional<int> askedId;
    QString askedHost;
    QObject::connect(
        &broker,
        &HostApprovalBroker::approvalRequested,
        &broker,
        [&](int id, const QString&, const QString& host, const QString&, const QString&) {
            askedId = id;
            askedHost = host;
        });
    auto answer = std::async(std::launch::async, [&] { return broker.ask(question()); });
    ASSERT_TRUE(spinUntil([&] { return askedId.has_value(); }));
    EXPECT_EQ(askedHost, QStringLiteral("api.example.com:443"));
    EXPECT_EQ(answer.wait_for(std::chrono::milliseconds{50}), std::future_status::timeout)
        << "the worker did not wait for the person";
    broker.answer(*askedId, true);
    EXPECT_EQ(answer.get(), ce::HostApprovalAnswer::Approve);
}

TEST_F(AppFixture, decline_all_releases_a_waiting_worker_with_no) {
    HostApprovalBroker broker;
    std::optional<int> askedId;
    QObject::connect(&broker,
                     &HostApprovalBroker::approvalRequested,
                     &broker,
                     [&](int id, const QString&, const QString&, const QString&, const QString&) {
                         askedId = id;
                     });
    auto answer = std::async(std::launch::async, [&] { return broker.ask(question()); });
    ASSERT_TRUE(spinUntil([&] { return askedId.has_value(); }));
    broker.declineAll();
    EXPECT_EQ(answer.get(), ce::HostApprovalAnswer::Deny);
    // Until reopened, later questions are declined without asking.
    EXPECT_EQ(std::async(std::launch::async, [&] { return broker.ask(question()); }).get(),
              ce::HostApprovalAnswer::Deny);
    broker.answer(*askedId, true);  // a late answer is ignored
}

TEST_F(AppFixture, asking_on_the_gui_thread_never_blocks) {
    HostApprovalBroker broker;
    EXPECT_EQ(broker.ask(question()), ce::HostApprovalAnswer::Unavailable);
}

TEST(QtRequestHostKey, agrees_with_the_engine_on_ordinary_urls) {
    EXPECT_EQ(qtRequestHostKey(QStringLiteral("https://IdP.example.com/token")).value_or("?"),
              "https://idp.example.com:443");
    EXPECT_EQ(qtRequestHostKey(QStringLiteral("http://127.0.0.1:8080/t")).value_or("?"),
              "http://127.0.0.1:8080");
    EXPECT_EQ(qtRequestHostKey(QStringLiteral("http://[::1]:9000/t")).value_or("?"),
              "http://[::1]:9000");
    EXPECT_EQ(qtRequestHostKey(QStringLiteral("https://a.example@b.example/t")).value_or("?"),
              "https://b.example:443");
}

TEST(QtRequestHostKey, unreadable_url_is_refused) {
    for (const char* url :
         {"", "not a url", "/token", "ftp://idp.example/token", "https://{{env.IDP}}/token"}) {
        EXPECT_FALSE(qtRequestHostKey(QString::fromUtf8(url)).has_value()) << url;
    }
}

TEST(QtRequestHostKey, a_url_the_two_parsers_read_differently_is_refused) {
    // libcurl reads the host as b.example; Qt's tolerant parser (the one the OAuth2 flow sends
    // with) reads a backslash as a path separator. Approving b.example must not send to a.example.
    EXPECT_TRUE(ce::hostKeyForUrl("http://a.example\\@b.example/t").has_value());
    EXPECT_FALSE(qtRequestHostKey(QStringLiteral("http://a.example\\@b.example/t")).has_value());

    // Each of these is read as a different host (or not at all) by one of the parsers; none may
    // come back as an approvable key unless both agree on it.
    for (const char* url : {"http://a.example\\@b.example/t",
                            "http://a.example%2F@b.example/t",
                            "http://a.example:80@b.example:81/t",
                            "http://b.example#@a.example/t"}) {
        const auto key = qtRequestHostKey(QString::fromUtf8(url));
        const auto curlKey = ce::hostKeyForUrl(url);
        if (key) {
            ASSERT_TRUE(curlKey.has_value()) << url;
            EXPECT_EQ(*key, *curlKey) << url;
            const QUrl qt{QString::fromUtf8(url)};
            EXPECT_EQ(QString::fromStdString(*key)
                          .section(QStringLiteral("://"), 1)
                          .section(QLatin1Char(':'), 0, 0),
                      qt.host().toLower())
                << url;
        }
    }
}

// ─── Through RunController and the engine ───────────────────────────────────

namespace {

/// An actor that signs in at 127.0.0.1:1 (refused at once), and an operation run as it.
[[nodiscard]] ce::Project actorProject() {
    ce::Project p;
    p.name = "Approvals";
    p.defaultEnvironment = "local";
    p.environments["local"] = {{"baseUrl", "http://127.0.0.1:1"}};
    ce::Actor user;
    user.id = ce::ActorId{"user"};
    user.strategy = ce::AuthStrategy::Simple;
    ce::AuthStep step;
    step.id = "login";
    step.method = ce::HttpMethod::Post;
    step.pathTemplate = "/login";
    step.bodyTemplate = R"({"password":"fake-desktop-password-0008"})";
    user.authSteps.push_back(std::move(step));
    p.actors[user.id] = std::move(user);
    ce::Resource res;
    res.id = ce::ResourceId{"item"};
    ce::Operation op;
    op.id = ce::OperationId{"item.authed"};
    op.resource = res.id;
    op.method = ce::HttpMethod::Get;
    op.pathTemplate = "/authed";
    op.actor = ce::ActorId{"user"};
    res.operations["authed"] = std::move(op);
    p.resources[res.id] = std::move(res);
    return p;
}

class RunApprovalFixture : public AppFixture {
protected:
    void SetUp() override {
        AppFixture::SetUp();
        ASSERT_TRUE(dir_.isValid());
        ASSERT_TRUE(ce::writeProject(fs::path{dir_.path().toStdString()}, actorProject(), true)
                        .has_value());
        project_.loadFromDirectory(dir_.path());
        ASSERT_TRUE(project_.hasProject());
        controller_ = std::make_unique<RunController>(engine_, project_, nullptr);
        QObject::connect(controller_.get(),
                         &RunController::runFinished,
                         &receiver_,
                         [this](const RunReport& report) { report_ = report; });
        QObject::connect(&controller_->hostApprovals(),
                         &HostApprovalBroker::approvalRequested,
                         &receiver_,
                         [this](int id,
                                const QString&,
                                const QString& host,
                                const QString& credential,
                                const QString&) {
                             askedId_ = id;
                             askedHost_ = host;
                             askedCredential_ = credential;
                         });
    }
    void TearDown() override {
        controller_.reset();
        QThreadPool::globalInstance()->waitForDone();
    }

    QTemporaryDir dir_;
    ProjectModel project_;
    ce::ExecutionEngine engine_{ce::makeDefaultDependencies()};
    std::unique_ptr<RunController> controller_;
    QObject receiver_;
    std::optional<RunReport> report_;
    std::optional<int> askedId_;
    QString askedHost_;
    QString askedCredential_;
};

}  // namespace

TEST_F(RunApprovalFixture, the_projects_own_base_url_host_is_asked_about_and_no_sends_nothing) {
    controller_->run(QStringLiteral("item.authed"), QString{}, false, false);
    ASSERT_TRUE(spinUntil([&] { return askedId_.has_value(); }));
    EXPECT_EQ(askedHost_, QStringLiteral("http://127.0.0.1:1"));
    EXPECT_TRUE(askedCredential_.contains(QStringLiteral("user")))
        << askedCredential_.toStdString();
    controller_->hostApprovals().answer(*askedId_, false);
    ASSERT_TRUE(spinUntil([&] { return report_.has_value(); }));
    ASSERT_FALSE(report_->steps.empty());
    EXPECT_EQ(report_->steps.back().error, ce::ErrorCode::HostNotApproved);
}

TEST_F(RunApprovalFixture, yes_is_remembered_for_the_next_run) {
    controller_->run(QStringLiteral("item.authed"), QString{}, false, false);
    ASSERT_TRUE(spinUntil([&] { return askedId_.has_value(); }));
    controller_->hostApprovals().answer(*askedId_, true);
    ASSERT_TRUE(spinUntil([&] { return report_.has_value(); }));
    // Sent: the sign-in then fails to connect, which is not a host refusal.
    ASSERT_FALSE(report_->steps.empty());
    EXPECT_NE(report_->steps.back().error, ce::ErrorCode::HostNotApproved);

    askedId_.reset();
    report_.reset();
    controller_->run(QStringLiteral("item.authed"), QString{}, false, false);
    ASSERT_TRUE(spinUntil([&] { return report_.has_value(); }));
    EXPECT_FALSE(askedId_.has_value()) << "asked again about an approved host";
}

TEST_F(RunApprovalFixture, cancelling_while_the_question_is_open_ends_the_run) {
    controller_->run(QStringLiteral("item.authed"), QString{}, false, false);
    ASSERT_TRUE(spinUntil([&] { return askedId_.has_value(); }));
    controller_->cancelRun();
    ASSERT_TRUE(spinUntil([&] { return report_.has_value(); }));
    EXPECT_NE(report_->outcome, ce::RunOutcome::Succeeded);
}

TEST_F(RunApprovalFixture, closing_while_the_question_is_open_does_not_hang) {
    controller_->run(QStringLiteral("item.authed"), QString{}, false, false);
    ASSERT_TRUE(spinUntil([&] { return askedId_.has_value(); }));
    QElapsedTimer timer;
    timer.start();
    controller_.reset();
    EXPECT_LT(timer.elapsed(), 5'000);
}

}  // namespace reqloom::desktop::tests
