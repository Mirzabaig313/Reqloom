// EngineStateTest — extraction cache lifecycle, skipped-step recording, cancellation and
// hook scoping, driven through the public engine with a scripted in-process HTTP client.
#include "infrastructure/hooks/HookRunner.h"
#include "infrastructure/http/HttpClient.h"
#include "infrastructure/schema/SchemaParser.h"
#include "infrastructure/secrets/SecretStore.h"
#include "infrastructure/storage/HistoryStore.h"
#include "support/HermeticDependencies.h"

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/ProjectTrustStore.h>
#include <reqloom/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <support/TempPath.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace ce = reqloom::engine;
namespace ct = reqloom::tests;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

/// Replies by URL path suffix from a per-route script; the last reply repeats.
class ScriptedHttpClient final : public ce::HttpClient {
public:
    void route(std::string pathSuffix, const int status, std::string body) {
        ce::HttpResponse response;
        response.status = status;
        response.body = std::move(body);
        routes_[std::move(pathSuffix)].push_back(std::move(response));
    }

    std::expected<ce::HttpResponse, ce::ReqloomError> send(
        const ce::HttpRequest& request) override {
        const std::lock_guard lock(mutex_);
        requests_.push_back(request);
        const auto path = request.url.substr(0, request.url.find('?'));
        std::string best{};
        for (const auto& [suffix, _] : routes_) {
            if (path.ends_with(suffix) && suffix.size() > best.size()) {
                best = suffix;
            }
        }
        if (best.empty()) {
            ce::HttpResponse notFound;
            notFound.status = 404;
            return notFound;
        }
        auto& script = routes_[best];
        auto& next = served_[best];
        const auto& reply = script[std::min(next, script.size() - 1)];
        ++next;
        return reply;
    }

    [[nodiscard]] std::vector<std::string> urls() const {
        const std::lock_guard lock(mutex_);
        std::vector<std::string> out;
        for (const auto& r : requests_) {
            out.push_back(r.url);
        }
        return out;
    }

    [[nodiscard]] std::vector<ce::HttpRequest> requests() const {
        const std::lock_guard lock(mutex_);
        return requests_;
    }

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::vector<ce::HttpResponse>> routes_;
    std::map<std::string, std::size_t> served_;
    std::vector<ce::HttpRequest> requests_;
};

/// In-memory secret store; `gate`, when set, blocks every read until it is fulfilled.
class GatedSecretStore final : public ce::SecretStore {
public:
    std::map<std::string, std::string> values;
    std::shared_future<void> gate;
    std::atomic<int> reads{0};

    std::expected<std::optional<std::string>, ce::ReqloomError> read(
        const std::string& name) override {
        ++reads;
        if (gate.valid()) {
            gate.wait();
        }
        if (const auto it = values.find(name); it != values.end()) {
            return std::optional<std::string>{it->second};
        }
        return std::optional<std::string>{};
    }
    std::expected<void, ce::ReqloomError> write(const std::string& name,
                                                const std::string& value) override {
        values[name] = value;
        return {};
    }
    std::expected<void, ce::ReqloomError> remove(const std::string& name) override {
        values.erase(name);
        return {};
    }
};

class MemoryTrustStore final : public ce::ProjectTrustStore {
public:
    std::expected<std::optional<fs::path>, ce::ReqloomError> folderFor(
        const std::string& projectId) override {
        if (const auto it = folders_.find(projectId); it != folders_.end()) {
            return std::optional<fs::path>{it->second};
        }
        return std::optional<fs::path>{};
    }
    std::expected<fs::path, ce::ReqloomError> recordIfAbsent(const std::string& projectId,
                                                             const fs::path& folder) override {
        return folders_.try_emplace(projectId, folder).first->second;
    }
    std::expected<void, ce::ReqloomError> rebind(const std::string& projectId,
                                                 const fs::path& folder) override {
        folders_[projectId] = folder;
        return {};
    }

private:
    std::map<std::string, fs::path> folders_;
};

class ScratchProject {
public:
    explicit ScratchProject(const std::string& yaml) {
        dir_ = ct::uniqueTempPath("reqloom-engine-state");
        fs::create_directories(dir_);
        std::ofstream{dir_ / "reqloom.yaml"} << yaml;
        auto parsed = ce::parseProject(dir_ / "reqloom.yaml");
        EXPECT_TRUE(parsed.has_value()) << (parsed ? "" : parsed.error().detail);
        if (parsed) {
            project = std::move(*parsed);
        }
    }
    ~ScratchProject() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    ScratchProject(const ScratchProject&) = delete;
    ScratchProject& operator=(const ScratchProject&) = delete;
    ScratchProject(ScratchProject&&) = delete;
    ScratchProject& operator=(ScratchProject&&) = delete;

    ce::Project project;

private:
    fs::path dir_;
};

/// Engine wired to fakes only: no network, no keychain, no history, real QuickJS hooks.
struct Rig {
    ScriptedHttpClient* http{nullptr};
    GatedSecretStore* secrets{nullptr};
    std::unique_ptr<ce::ExecutionEngine> engine;

    Rig() {
        auto httpOwned = std::make_unique<ScriptedHttpClient>();
        auto secretsOwned = std::make_unique<GatedSecretStore>();
        http = httpOwned.get();
        secrets = secretsOwned.get();
        ce::ExecutionEngine::Dependencies deps{std::move(httpOwned),
                                               nullptr,
                                               nullptr,
                                               std::move(secretsOwned),
                                               ce::makeQuickJsHookRunner()};
        deps.trust = std::make_unique<MemoryTrustStore>();
        deps.hostApprovals = std::make_unique<reqloom::tests::MemoryHostApprovals>(true);
        engine = std::make_unique<ce::ExecutionEngine>(std::move(deps));
    }
};

[[nodiscard]] const ce::StepResult* stepFor(const ce::RunResult& result, const std::string& op) {
    for (const auto& step : result.steps) {
        if (step.op.value == op && !step.forEachIndex && !step.pollAttempt) {
            return &step;
        }
    }
    return nullptr;
}

constexpr const char* kListYaml = R"YAML(
version: 1
name: EngineState
default_environment: local
environment:
  baseUrl: http://placeholder
resources:
  org:
    operations:
      list:
        method: GET
        path: /orgs
        expect_status: 200
        extract:
          org_id: $.items[*].id
  org_detail:
    operations:
      get:
        method: GET
        path: /orgs/{{org.org_id}}
        expect_status: 200
        for_each:
          over: org
)YAML";

constexpr const char* kChainYaml = R"YAML(
version: 1
name: EngineState
default_environment: local
environment:
  baseUrl: http://placeholder
resources:
  user:
    operations:
      create:
        method: POST
        path: /users
        expect_status: 200
        extract:
          user_id: $.id
      verify:
        method: POST
        path: /users/{{user.user_id}}/verify
        expect_status: 200
        extract:
          verified_at: $.at
  profile:
    operations:
      get:
        method: GET
        path: /profiles/{{user.user_id}}
        expect_status: 200
)YAML";

}  // namespace

TEST(EngineState, rerunning_list_producer_replaces_its_items_instead_of_appending) {
    ScratchProject scratch(kListYaml);
    Rig rig;
    rig.http->route("/orgs", 200, R"({"items":[{"id":"a"},{"id":"b"},{"id":"c"}]})");
    rig.http->route("/orgs/a", 200, "{}");
    rig.http->route("/orgs/b", 200, "{}");
    rig.http->route("/orgs/c", 200, "{}");
    ce::RunContext ctx;

    ASSERT_TRUE(rig.engine->run(scratch.project, ce::OperationId{"org.list"}, ctx));
    ASSERT_TRUE(rig.engine->run(scratch.project, ce::OperationId{"org.list"}, ctx));
    const auto detail = rig.engine->run(scratch.project, ce::OperationId{"org_detail.get"}, ctx);

    EXPECT_EQ(ctx.instances(ce::ResourceId{"org"}).size(), 3U);
    ASSERT_TRUE(detail.has_value());
    std::size_t iterations{};
    for (const auto& step : detail->steps) {
        iterations += step.forEachIndex.has_value() ? 1U : 0U;
    }
    EXPECT_EQ(iterations, 3U);
}

TEST(EngineState, rerunning_producer_keeps_instances_of_other_operations) {
    ScratchProject scratch(kChainYaml);
    Rig rig;
    rig.http->route("/users", 200, R"({"id":"u1"})");
    rig.http->route("/users", 200, R"({"id":"u2"})");
    rig.http->route("/verify", 200, R"({"at":"t1"})");
    ce::RunContext ctx;

    ASSERT_TRUE(rig.engine->run(scratch.project, ce::OperationId{"user.verify"}, ctx));
    ASSERT_TRUE(rig.engine->run(scratch.project, ce::OperationId{"user.create"}, ctx));

    const auto& users = ctx.instances(ce::ResourceId{"user"});
    ASSERT_EQ(users.size(), 2U);
    EXPECT_EQ(users[0].producer.value, "user.verify");
    EXPECT_EQ(users[1].producer.value, "user.create");
    EXPECT_EQ(users[1].variables.at("user_id"), "u2");
}

TEST(EngineState, null_extraction_on_rerun_reports_unresolved_instead_of_older_value) {
    ScratchProject scratch(kChainYaml);
    Rig rig;
    rig.http->route("/users", 200, R"({"id":"u1"})");
    rig.http->route("/users", 200, R"({"id":null})");
    rig.http->route("/profiles/u1", 200, "{}");
    ce::RunContext ctx;
    ASSERT_TRUE(rig.engine->run(scratch.project, ce::OperationId{"profile.get"}, ctx));
    const auto rerun = rig.engine->run(scratch.project, ce::OperationId{"user.create"}, ctx);
    ASSERT_TRUE(rerun.has_value());
    EXPECT_TRUE(rerun->succeeded()) << "a null extraction does not fail its own step";

    const auto later = rig.engine->run(scratch.project, ce::OperationId{"profile.get"}, ctx);

    ASSERT_TRUE(later.has_value());
    const auto* profile = stepFor(*later, "profile.get");
    ASSERT_NE(profile, nullptr);
    EXPECT_EQ(profile->status, ce::StepResult::Status::Failed);
    EXPECT_EQ(profile->error, ce::ErrorCode::VarUnresolved);
    EXPECT_NE(profile->detail.find("The last user.create response returned null for 'user_id'"),
              std::string::npos)
        << profile->detail;
    std::size_t profileRequests{};
    for (const auto& url : rig.http->urls()) {
        profileRequests += url.ends_with("/profiles/u1") ? 1U : 0U;
    }
    EXPECT_EQ(profileRequests, 1U);
}

TEST(EngineState, skipped_step_is_recorded_in_context) {
    ScratchProject scratch(kChainYaml);
    Rig rig;
    rig.http->route("/users", 200, R"({"id":"u1"})");
    rig.http->route("/profiles/u1", 200, "{}");
    ce::RunContext ctx;
    ASSERT_TRUE(rig.engine->run(scratch.project, ce::OperationId{"profile.get"}, ctx));

    ASSERT_TRUE(rig.engine->run(scratch.project, ce::OperationId{"profile.get"}, ctx));

    std::size_t skipped{};
    for (const auto& step : ctx.steps()) {
        if (step.op.value == "user.create" && step.status == ce::StepResult::Status::Skipped) {
            ++skipped;
        }
    }
    EXPECT_EQ(skipped, 1U);
}

namespace {

/// Runs `target` on a worker; `trigger` decides from each event when to cancel.
struct CancelProbe {
    std::promise<ce::RunId> cancelAt;
    std::once_flag once;
    std::atomic<std::uint64_t> runId{0};
};

[[nodiscard]] ce::RunOutcome runAndCancel(Rig& rig,
                                          const ce::Project& project,
                                          const std::string& target,
                                          const std::function<bool(const ce::RunEvent&)>& trigger,
                                          std::chrono::milliseconds& elapsed) {
    auto probe = std::make_shared<CancelProbe>();
    rig.engine->subscribe([probe, trigger](const ce::RunEvent& event) {
        if (const auto* started = std::get_if<ce::RunStarted>(&event)) {
            probe->runId = started->runId.value;
        }
        if (trigger(event)) {
            std::call_once(probe->once,
                           [&] { probe->cancelAt.set_value(ce::RunId{probe->runId.load()}); });
        }
    });
    ce::RunContext ctx;
    const auto start = std::chrono::steady_clock::now();
    auto worker = std::async(
        std::launch::async, [&] { return rig.engine->run(project, ce::OperationId{target}, ctx); });
    const auto id = probe->cancelAt.get_future().get();
    std::this_thread::sleep_for(100ms);
    rig.engine->cancel(id);
    const auto result = worker.get();
    elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    EXPECT_TRUE(result.has_value());
    return result ? result->outcome : ce::RunOutcome::Failed;
}

}  // namespace

TEST(EngineState, cancel_wakes_a_retry_backoff_at_once) {
    ScratchProject scratch(R"YAML(
version: 1
name: EngineState
default_environment: local
environment:
  baseUrl: http://placeholder
resources:
  item:
    operations:
      get:
        method: GET
        path: /flaky
        expect_status: 200
        retry: { max: 3, backoff: 20000 }
)YAML");
    Rig rig;
    rig.http->route("/flaky", 503, "{}");
    std::chrono::milliseconds elapsed{};

    const auto outcome = runAndCancel(
        rig,
        scratch.project,
        "item.get",
        [](const ce::RunEvent& e) { return std::holds_alternative<ce::ResponseReceived>(e); },
        elapsed);

    EXPECT_EQ(outcome, ce::RunOutcome::Cancelled);
    EXPECT_LT(elapsed, 5s);
    EXPECT_EQ(rig.http->urls().size(), 1U);
}

TEST(EngineState, cancel_wakes_a_poll_wait_at_once) {
    ScratchProject scratch(R"YAML(
version: 1
name: EngineState
default_environment: local
environment:
  baseUrl: http://placeholder
resources:
  job:
    operations:
      start:
        method: POST
        path: /jobs
        expect_status: 202
        poll_until:
          path: /jobs/1
          success_when: $.done == true
          interval: 30s
          timeout: 120s
)YAML");
    Rig rig;
    rig.http->route("/jobs", 202, "{}");
    rig.http->route("/jobs/1", 200, R"({"done":false})");
    int responses{};
    std::chrono::milliseconds elapsed{};

    const auto outcome = runAndCancel(
        rig,
        scratch.project,
        "job.start",
        [&responses](const ce::RunEvent& e) {
            return std::holds_alternative<ce::ResponseReceived>(e) && ++responses == 2;
        },
        elapsed);

    EXPECT_EQ(outcome, ce::RunOutcome::Cancelled);
    EXPECT_LT(elapsed, 5s);
}

TEST(EngineState, cancel_reaches_a_run_waiting_on_the_secret_store) {
    ScratchProject scratch(R"YAML(
version: 1
name: EngineState
id: engine-state-secrets
default_environment: local
environment:
  baseUrl: http://placeholder
resources:
  item:
    operations:
      get:
        method: GET
        path: /items
        headers:
          X-Key: "{{secret.API_KEY}}"
        expect_status: 200
)YAML");
    Rig rig;
    rig.http->route("/items", 200, "{}");
    std::promise<void> release;
    rig.secrets->gate = release.get_future().share();
    std::promise<ce::RunId> started;
    rig.engine->subscribe([&started](const ce::RunEvent& e) {
        if (const auto* s = std::get_if<ce::RunStarted>(&e)) {
            started.set_value(s->runId);
        }
    });
    ce::RunContext ctx;
    auto worker = std::async(std::launch::async, [&] {
        return rig.engine->run(scratch.project, ce::OperationId{"item.get"}, ctx);
    });

    rig.engine->cancel(started.get_future().get());
    release.set_value();
    const auto result = worker.get();

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(result->outcome, ce::RunOutcome::Cancelled);
    EXPECT_TRUE(rig.http->urls().empty());
}

TEST(EngineState, cancel_during_401_recovery_skips_the_reauthentication) {
    ScratchProject scratch(R"YAML(
version: 1
name: EngineState
default_environment: local
environment:
  baseUrl: http://placeholder
actors:
  user:
    auth:
      strategy: simple
      method: POST
      path: /login
      expect_status: 200
      extract:
        token: $.token
    inject:
      headers:
        Authorization: "Bearer {{user.token}}"
resources:
  item:
    operations:
      get:
        method: GET
        path: /items
        actor: user
        expect_status: 200
)YAML");
    Rig rig;
    rig.http->route("/login", 200, R"({"token":"t"})");
    rig.http->route("/items", 401, "{}");
    std::atomic<std::uint64_t> runId{0};
    auto* engine = rig.engine.get();
    rig.engine->subscribe([&runId, engine](const ce::RunEvent& e) {
        if (const auto* s = std::get_if<ce::RunStarted>(&e)) {
            runId = s->runId.value;
        } else if (const auto* r = std::get_if<ce::ResponseReceived>(&e); r && r->status == 401) {
            engine->cancel(ce::RunId{runId.load()});
        }
    });
    ce::RunContext ctx;

    const auto result = rig.engine->run(scratch.project, ce::OperationId{"item.get"}, ctx);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->outcome, ce::RunOutcome::Cancelled);
    std::size_t logins{};
    for (const auto& url : rig.http->urls()) {
        logins += url.ends_with("/login") ? 1U : 0U;
    }
    EXPECT_EQ(logins, 1U);
}

TEST(EngineState, hook_sees_only_its_operations_secrets_and_actor) {
    ScratchProject scratch(R"YAML(
version: 1
name: EngineState
id: engine-state-hooks
default_environment: local
environment:
  baseUrl: http://placeholder
  via_env: !secret VIA_ENV
actors:
  alice:
    auth:
      strategy: simple
      method: POST
      path: /login/alice
      expect_status: 200
      extract:
        token: $.token
  bob:
    auth:
      strategy: simple
      method: POST
      path: /login/bob
      expect_status: 200
      extract:
        token: $.token
resources:
  thing:
    operations:
      make:
        method: POST
        path: /things
        actor: bob
        headers:
          X-Other: "{{secret.OTHER}}"
        expect_status: 200
        extract:
          id: $.id
  probe:
    operations:
      send:
        method: GET
        path: /probe/{{thing.id}}
        actor: alice
        headers:
          X-Own: "{{secret.OWN}}"
          X-Env: "{{env.via_env}}"
        expect_status: 200
        pre_request: |
          const named = ctx.secret.SCRIPT;
          ctx.request.headers['X-Seen'] =
              Object.keys(ctx.secret).sort().join(',') + '|' +
              Object.keys(ctx.actors).sort().join(',');
)YAML");
    Rig rig;
    for (const auto* name : {"OWN", "OTHER", "SCRIPT", "VIA_ENV", "UNUSED"}) {
        rig.secrets->values[std::string{"engine-state-hooks/"} + name] = std::string{"v-"} + name;
    }
    rig.http->route("/login/alice", 200, R"({"token":"ta"})");
    rig.http->route("/login/bob", 200, R"({"token":"tb"})");
    rig.http->route("/things", 200, R"({"id":"t1"})");
    rig.http->route("/probe/t1", 200, "{}");
    ce::RunContext ctx;

    const auto result = rig.engine->run(scratch.project, ce::OperationId{"probe.send"}, ctx);

    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->succeeded());
    std::string seen{};
    for (const auto& request : rig.http->requests()) {
        if (const auto it = request.headers.find("X-Seen"); it != request.headers.end()) {
            seen = it->second;
        }
    }
    EXPECT_EQ(seen, "OWN,SCRIPT,VIA_ENV|alice");
}

namespace {

/// `order.create` extracts id and name; `order.check` asserts `predicate` against /check.
[[nodiscard]] std::string predicateYaml(const std::string& predicate) {
    return R"YAML(
version: 1
name: EngineState
id: engine-state-predicates
default_environment: local
environment:
  baseUrl: http://placeholder
resources:
  order:
    operations:
      create:
        method: POST
        path: /orders
        expect_status: 200
        extract:
          order_id: $.id
          order_name: $.name
      check:
        method: GET
        path: /check
        expect_status: 200
        assert:
          - ')YAML" +
           predicate + R"YAML('
)YAML";
}

struct PredicateRun {
    std::expected<ce::RunResult, ce::ReqloomError> result{
        std::unexpected(ce::ReqloomError{ce::ErrorCode::Internal, ce::ErrorClass::Run, ""})};
    std::vector<ce::AssertionCompleted> assertions;

    [[nodiscard]] const ce::StepResult* check() const {
        return result ? stepFor(*result, "order.check") : nullptr;
    }
};

[[nodiscard]] PredicateRun runCheck(const std::string& predicate,
                                    const std::string& created,
                                    const std::string& checked,
                                    std::map<std::string, std::string> secrets = {}) {
    ScratchProject scratch(predicateYaml(predicate));
    Rig rig;
    for (auto& [name, value] : secrets) {
        rig.secrets->values["engine-state-predicates/" + name] = value;
    }
    rig.http->route("/orders", 200, created);
    rig.http->route("/check", 200, checked);
    PredicateRun out;
    rig.engine->subscribe([&out](const ce::RunEvent& e) {
        if (const auto* a = std::get_if<ce::AssertionCompleted>(&e)) {
            out.assertions.push_back(*a);
        }
    });
    ce::RunContext ctx;
    out.result = rig.engine->run(scratch.project, ce::OperationId{"order.check"}, ctx);
    return out;
}

}  // namespace

TEST(PredicateTemplates, numeric_placeholder_compares_as_a_number_and_adds_the_producer) {
    const auto pass =
        runCheck("$.id == {{order.order_id}}", R"({"id":42,"name":"n"})", R"({"id":42})");
    const auto fail =
        runCheck("$.id == {{order.order_id}}", R"({"id":42,"name":"n"})", R"({"id":43})");

    ASSERT_NE(pass.check(), nullptr);
    EXPECT_EQ(pass.check()->status, ce::StepResult::Status::Succeeded) << pass.check()->detail;
    ASSERT_NE(stepFor(*pass.result, "order.create"), nullptr) << "the assertion adds the edge";
    ASSERT_NE(fail.check(), nullptr);
    EXPECT_EQ(fail.check()->error, ce::ErrorCode::AssertionFailed);
}

TEST(PredicateTemplates, quoted_placeholder_with_a_quote_in_the_value_stays_one_string) {
    const auto out = runCheck(R"($.name == "{{order.order_name}}")",
                              R"({"id":1,"name":"O\"Brien's"})",
                              R"({"name":"O\"Brien's"})");

    ASSERT_NE(out.check(), nullptr);
    EXPECT_EQ(out.check()->status, ce::StepResult::Status::Succeeded) << out.check()->detail;
}

TEST(PredicateTemplates, value_that_looks_like_operators_cannot_make_an_assertion_pass) {
    const auto quoted = runCheck(R"($.name == "{{order.order_name}}")",
                                 R"({"id":1,"name":"\" || true || \""})",
                                 R"({"name":"someone else"})");
    const auto bare = runCheck("$.name == {{order.order_name}}",
                               R"({"id":1,"name":"1 || true"})",
                               R"({"name":"someone else"})");

    ASSERT_NE(quoted.check(), nullptr);
    EXPECT_EQ(quoted.check()->error, ce::ErrorCode::AssertionFailed);
    ASSERT_NE(bare.check(), nullptr);
    EXPECT_EQ(bare.check()->error, ce::ErrorCode::AssertionFailed);
}

TEST(PredicateTemplates, unresolved_placeholder_fails_the_assertion_naming_the_reference) {
    const auto out = runCheck("$.id == {{env.not_set}}", R"({"id":1,"name":"n"})", R"({"id":1})");

    ASSERT_NE(out.check(), nullptr);
    EXPECT_EQ(out.check()->error, ce::ErrorCode::VarUnresolved);
    EXPECT_NE(out.check()->detail.find("{{env.not_set}}"), std::string::npos)
        << out.check()->detail;
}

TEST(PredicateTemplates, secret_in_an_assertion_is_loaded_and_never_emitted) {
    const auto out = runCheck(R"($.key == "{{secret.CHECK_KEY}}")",
                              R"({"id":1,"name":"n"})",
                              R"({"key":"sk-check-123"})",
                              {{"CHECK_KEY", "sk-check-123"}});

    ASSERT_NE(out.check(), nullptr);
    EXPECT_EQ(out.check()->status, ce::StepResult::Status::Succeeded) << out.check()->detail;
    ASSERT_EQ(out.assertions.size(), 1U);
    EXPECT_EQ(out.assertions.front().expr, R"($.key == "{{secret.CHECK_KEY}}")");
    EXPECT_EQ(out.assertions.front().name.find("sk-check-123"), std::string::npos);
}

TEST(PredicateTemplates, poll_condition_placeholder_is_substituted) {
    ScratchProject scratch(R"YAML(
version: 1
name: EngineState
default_environment: local
environment:
  baseUrl: http://placeholder
  wanted: done
resources:
  job:
    operations:
      start:
        method: POST
        path: /jobs
        expect_status: 202
        poll_until:
          path: /jobs/1
          success_when: '$.state == "{{env.wanted}}"'
          interval: 10ms
          max_attempts: 3
)YAML");
    Rig rig;
    rig.http->route("/jobs", 202, "{}");
    rig.http->route("/jobs/1", 200, R"({"state":"running"})");
    rig.http->route("/jobs/1", 200, R"({"state":"done"})");
    ce::RunContext ctx;

    const auto result = rig.engine->run(scratch.project, ce::OperationId{"job.start"}, ctx);

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->succeeded());
}

TEST(PredicateTemplates, syntax_error_around_a_placeholder_is_reported_at_load) {
    const auto dir = ct::uniqueTempPath("reqloom-engine-state-load");
    fs::create_directories(dir);
    std::ofstream{dir / "reqloom.yaml"} << predicateYaml("$.id == {{order.order_id}} &&");

    const auto parsed = ce::parseProject(dir / "reqloom.yaml");
    const bool rejected = !parsed || !ce::validateProject(*parsed);

    EXPECT_TRUE(rejected);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(PredicateTemplates, secret_referenced_only_by_an_assertion_is_collected) {
    ScratchProject scratch(predicateYaml(R"($.key == "{{secret.ONLY_HERE}}")"));

    const auto names = ce::collectSecretReferences(scratch.project);

    EXPECT_NE(std::find(names.begin(), names.end(), "ONLY_HERE"), names.end());
}
