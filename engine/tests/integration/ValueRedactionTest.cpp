// ValueRedactionTest — secret VALUES never leave the engine in clear text.
//
// Name-based masking alone let `X-Custom: {{secret.API_KEY}}`, an extracted
// login token stored as `jwt`, or an allowed `{{$.env.NAME}}` value reach
// events and run history verbatim. These tests drive the engine with a fake
// HTTP client, a recording history store and an event subscriber, and assert
// that no registered value appears in any emitted event, stored row or step
// detail — while the request actually sent still carries the real value.
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

#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace ce = reqloom::engine;
namespace ct = reqloom::tests;
namespace fs = std::filesystem;

namespace {

/// Replies by URL path suffix; records every request it is given.
class RoutedHttpClient final : public ce::HttpClient {
public:
    void route(std::string pathSuffix, const int status, std::string body) {
        ce::HttpResponse response;
        response.status = status;
        response.body = std::move(body);
        routes_.emplace_back(std::move(pathSuffix), std::move(response));
    }

    std::expected<ce::HttpResponse, ce::ReqloomError> send(
        const ce::HttpRequest& request) override {
        requests_.push_back(request);
        const auto path = request.url.substr(0, request.url.find('?'));
        for (const auto& [suffix, response] : routes_) {
            if (path.ends_with(suffix)) {
                return response;
            }
        }
        ce::HttpResponse notFound;
        notFound.status = 404;
        return notFound;
    }

    [[nodiscard]] const std::vector<ce::HttpRequest>& requests() const noexcept {
        return requests_;
    }

private:
    std::vector<std::pair<std::string, ce::HttpResponse>> routes_;
    std::vector<ce::HttpRequest> requests_;
};

class MapSecretStore final : public ce::SecretStore {
public:
    void put(std::string name, std::string value) { values_[std::move(name)] = std::move(value); }

    std::expected<std::optional<std::string>, ce::ReqloomError> read(
        const std::string& name) override {
        if (const auto it = values_.find(name); it != values_.end()) {
            return std::optional<std::string>{it->second};
        }
        return std::optional<std::string>{};
    }
    std::expected<void, ce::ReqloomError> write(const std::string& name,
                                                const std::string& value) override {
        values_[name] = value;
        return {};
    }
    std::expected<void, ce::ReqloomError> remove(const std::string& name) override {
        values_.erase(name);
        return {};
    }

private:
    std::map<std::string, std::string> values_;
};

/// Trust-on-first-use folder record, in memory.
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

/// Keeps every appended event, standing in for the SQLite history store.
class RecordingHistoryStore final : public ce::HistoryStore {
public:
    explicit RecordingHistoryStore(std::vector<ce::RunEvent>* sink) : sink_(sink) {}
    std::expected<void, ce::ReqloomError> open(const fs::path&) override { return {}; }
    std::expected<void, ce::ReqloomError> append(const ce::RunEvent& event) override {
        sink_->push_back(event);
        return {};
    }
    [[nodiscard]] std::expected<std::vector<ce::RunEvent>, ce::ReqloomError> eventsFor(
        ce::RunId) const override {
        return *sink_;
    }
    [[nodiscard]] std::expected<std::vector<ce::RunHistoryRow>, ce::ReqloomError> listRuns(
        std::size_t) const override {
        return std::vector<ce::RunHistoryRow>{};
    }
    std::expected<void, ce::ReqloomError> clear() override {
        sink_->clear();
        return {};
    }
    void close() override {}

private:
    std::vector<ce::RunEvent>* sink_;
};

/// Every masked free-text field of an event, concatenated, so a test can
/// assert a value appears nowhere in it. Captured bodies are excluded: they
/// are the person's own view and stay as received (see `capturedBodies`).
[[nodiscard]] std::string eventText(const ce::RunEvent& event) {
    std::string out{};
    const auto headers = [&](const std::vector<std::pair<std::string, std::string>>& hs) {
        for (const auto& [k, v] : hs) {
            out += k + ": " + v + "\n";
        }
    };
    if (const auto* e = std::get_if<ce::RunStarted>(&event)) {
        out += e->envName;
    } else if (const auto* pr = std::get_if<ce::RequestPrepared>(&event)) {
        out += pr->url + "\n";
        headers(pr->maskedHeaders);
    } else if (const auto* rr = std::get_if<ce::ResponseReceived>(&event)) {
        headers(rr->headers);
    } else if (const auto* ex = std::get_if<ce::ExtractionCompleted>(&event)) {
        out += ex->variableName + "=" + ex->value + " (" + ex->sourcePath + ")";
    } else if (const auto* as = std::get_if<ce::AssertionCompleted>(&event)) {
        out += as->name + " " + as->expr + " " + as->detail;
    } else if (const auto* sf = std::get_if<ce::StepFailed>(&event)) {
        out += sf->detail;
        for (const auto& d : sf->diagnostics) {
            out += d.token + d.useName + d.sourceId + d.sourceField;
        }
    }
    return out;
}

class ScratchProject {
public:
    explicit ScratchProject(const std::string& yaml) {
        dir_ = ct::uniqueTempPath("reqloom-value-redaction");
        fs::create_directories(dir_);
        std::ofstream{dir_ / "reqloom.yaml"} << yaml;
    }
    ~ScratchProject() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    ScratchProject(const ScratchProject&) = delete;
    ScratchProject& operator=(const ScratchProject&) = delete;
    ScratchProject(ScratchProject&&) = delete;
    ScratchProject& operator=(ScratchProject&&) = delete;
    [[nodiscard]] fs::path yaml() const { return dir_ / "reqloom.yaml"; }

private:
    fs::path dir_;
};

struct Outcome {
    std::expected<ce::RunResult, ce::ReqloomError> result{
        std::unexpected(ce::ReqloomError{ce::ErrorCode::Internal, ce::ErrorClass::Run, ""})};
    std::vector<ce::RunEvent> emitted;
    std::vector<ce::RunEvent> stored;
    std::vector<ce::HttpRequest> sent;

    /// Captured response bodies, emitted and stored, in order.
    [[nodiscard]] std::vector<std::string> capturedBodies() const {
        std::vector<std::string> bodies{};
        for (const auto* events : {&emitted, &stored}) {
            for (const auto& e : *events) {
                if (const auto* rr = std::get_if<ce::ResponseReceived>(&e); rr && rr->body) {
                    bodies.push_back(*rr->body);
                }
            }
        }
        return bodies;
    }

    /// Every event (emitted and stored) and every step detail as one string.
    [[nodiscard]] std::string everything() const {
        std::string all{};
        for (const auto& e : emitted) {
            all += eventText(e) + "\n";
        }
        for (const auto& e : stored) {
            all += eventText(e) + "\n";
        }
        if (result) {
            for (const auto& s : result->steps) {
                all += s.detail + "\n";
                for (const auto& a : s.assertions) {
                    all += a.name + " " + a.expr + " " + a.detail + "\n";
                }
            }
        }
        return all;
    }

    [[nodiscard]] const ce::StepResult* failedStep() const {
        if (!result) {
            return nullptr;
        }
        for (const auto& s : result->steps) {
            if (s.status == ce::StepResult::Status::Failed) {
                return &s;
            }
        }
        return nullptr;
    }
};

struct RunSetup {
    std::function<void(RoutedHttpClient&)> routes;
    std::map<std::string, std::string> secrets;
    ce::RunOptions options{};
};

[[nodiscard]] Outcome runProject(const std::string& yaml,
                                 const std::string& target,
                                 const RunSetup& setup) {
    ScratchProject scratch(yaml);
    auto project = ce::parseProject(scratch.yaml());
    EXPECT_TRUE(project.has_value()) << (project ? "" : project.error().detail);
    Outcome out;
    if (!project) {
        return out;
    }
    auto http = std::make_unique<RoutedHttpClient>();
    auto* httpRaw = http.get();
    setup.routes(*httpRaw);
    auto secrets = std::make_unique<MapSecretStore>();
    for (const auto& [k, v] : setup.secrets) {
        secrets->put(k, v);
    }
    auto history = std::make_unique<RecordingHistoryStore>(&out.stored);
    ce::ExecutionEngine::Dependencies deps{std::move(http),
                                           nullptr,
                                           std::move(history),
                                           setup.secrets.empty() ? nullptr : std::move(secrets),
                                           nullptr};
    deps.trust = std::make_unique<MemoryTrustStore>();
    deps.hostApprovals = std::make_unique<reqloom::tests::MemoryHostApprovals>(true);
    ce::ExecutionEngine engine{std::move(deps)};
    engine.subscribe([&out](const ce::RunEvent& e) { out.emitted.push_back(e); });
    ce::RunContext ctx;
    out.result = engine.run(*project, ce::OperationId{target}, ctx, setup.options);
    out.sent = httpRaw->requests();
    return out;
}

[[nodiscard]] bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

[[nodiscard]] std::string sentHeader(const Outcome& out, std::string_view name) {
    for (const auto& req : out.sent) {
        if (const auto it = req.headers.find(std::string{name}); it != req.headers.end()) {
            return it->second;
        }
    }
    return {};
}

constexpr const char* kSecretHeaderYaml = R"YAML(
version: 1
name: ValueRedaction
id: value-redaction-test
default_environment: local
environment:
  baseUrl: http://placeholder
resources:
  item:
    operations:
      read:
        method: GET
        path: /items
        headers:
          X-Custom: "{{secret.API_KEY}}"
        expect_status: 200
)YAML";

constexpr std::string_view kApiKey = "sk_live_9f8e7d6c5b4a";

}  // namespace

TEST(ValueRedaction, secret_in_non_sensitive_header_never_reaches_events_or_history) {
    RunSetup setup;
    setup.secrets["value-redaction-test/API_KEY"] = std::string{kApiKey};
    setup.options.captureResponseBodies = true;
    setup.routes = [](RoutedHttpClient& http) {
        http.route("/items", 401, R"({"error":"key sk_live_9f8e7d6c5b4a is revoked"})");
    };
    const auto out = runProject(kSecretHeaderYaml, "item.read", setup);
    ASSERT_TRUE(out.result.has_value()) << out.result.error().detail;

    EXPECT_EQ(sentHeader(out, "X-Custom"), kApiKey) << "the request sent must be unchanged";
    ASSERT_FALSE(out.emitted.empty());
    ASSERT_FALSE(out.stored.empty());
    EXPECT_FALSE(contains(out.everything(), kApiKey)) << out.everything();

    const auto* failed = out.failedStep();
    ASSERT_NE(failed, nullptr);
    EXPECT_TRUE(contains(failed->detail, "is revoked")) << failed->detail;
    EXPECT_TRUE(contains(failed->detail, "<redacted>")) << failed->detail;

    // The same run's captured body reaches the subscriber and history as received.
    const auto bodies = out.capturedBodies();
    ASSERT_EQ(bodies.size(), 2U);
    for (const auto& body : bodies) {
        EXPECT_EQ(body, R"({"error":"key sk_live_9f8e7d6c5b4a is revoked"})");
    }
}

TEST(ValueRedaction, an_assertion_against_a_secret_explains_the_failure_without_revealing_it) {
    constexpr const char* kYaml = R"YAML(
version: 1
name: AssertionRedaction
id: value-redaction-test
default_environment: local
environment:
  baseUrl: http://placeholder
resources:
  item:
    operations:
      read:
        method: GET
        path: /items
        expect_status: 200
        assert:
          - $.echo == "{{secret.API_KEY}}"
)YAML";
    RunSetup setup;
    setup.secrets["value-redaction-test/API_KEY"] = std::string{kApiKey};
    setup.routes = [](RoutedHttpClient& http) {
        http.route("/items", 200, R"({"echo":"wrong"})");
    };
    const auto out = runProject(kYaml, "item.read", setup);
    ASSERT_TRUE(out.result.has_value()) << out.result.error().detail;

    const auto* failed = out.failedStep();
    ASSERT_NE(failed, nullptr);
    ASSERT_EQ(failed->assertions.size(), 1U);
    EXPECT_EQ(failed->assertions[0].detail, R"($.echo was "wrong", expected == "<redacted>")");
    std::size_t explainedEvents = 0;
    for (const auto* events : {&out.emitted, &out.stored}) {
        for (const auto& e : *events) {
            if (const auto* as = std::get_if<ce::AssertionCompleted>(&e)) {
                EXPECT_EQ(as->detail, failed->assertions[0].detail);
                ++explainedEvents;
            }
        }
    }
    EXPECT_EQ(explainedEvents, 2U) << "one emitted, one stored";
    EXPECT_FALSE(contains(out.everything(), kApiKey)) << out.everything();
}

TEST(ValueRedaction, captured_body_is_unmasked_while_url_headers_and_extractions_stay_masked) {
    constexpr const char* kYaml = R"YAML(
version: 1
name: CapturedBodyMasking
id: value-redaction-test
default_environment: local
environment:
  baseUrl: http://placeholder
resources:
  item:
    operations:
      read:
        method: GET
        path: /items?key={{secret.API_KEY}}
        headers:
          X-Custom: "{{secret.API_KEY}}"
        expect_status: 200
        extract:
          echoed: $.echo
)YAML";
    constexpr std::string_view kBody =
        R"({"access_token":"tok-abc123def456","echo":"sk_live_9f8e7d6c5b4a"})";
    RunSetup setup;
    setup.secrets["value-redaction-test/API_KEY"] = std::string{kApiKey};
    setup.options.captureResponseBodies = true;
    setup.routes = [&](RoutedHttpClient& http) {
        http.route("/items", 200, std::string{kBody});
    };
    const auto out = runProject(kYaml, "item.read", setup);
    ASSERT_TRUE(out.result.has_value()) << out.result.error().detail;

    const auto bodies = out.capturedBodies();
    ASSERT_EQ(bodies.size(), 2U) << "one emitted, one stored";
    for (const auto& body : bodies) {
        EXPECT_EQ(body, kBody);
    }
    bool sawExtraction = false;
    for (const auto* events : {&out.emitted, &out.stored}) {
        for (const auto& e : *events) {
            if (std::holds_alternative<ce::ExtractionCompleted>(e)) {
                sawExtraction = true;
            }
        }
    }
    EXPECT_TRUE(sawExtraction);
    const auto all = out.everything();
    EXPECT_FALSE(contains(all, kApiKey)) << all;
}

TEST(ValueRedaction, allowed_env_value_is_redacted_from_events) {
    constexpr const char* kEnvYaml = R"YAML(
version: 1
name: EnvRedaction
default_environment: local
allowed_env: [REQLOOM_VALUE_REDACTION_TEST]
environment:
  baseUrl: http://placeholder
resources:
  item:
    operations:
      read:
        method: GET
        path: /items
        headers:
          X-Env: "{{$.env.REQLOOM_VALUE_REDACTION_TEST}}"
        expect_status: 200
)YAML";
    constexpr std::string_view kEnvValue = "env-value-31337-abc";
#ifdef _WIN32
    _putenv_s("REQLOOM_VALUE_REDACTION_TEST", std::string{kEnvValue}.c_str());
#else
    setenv("REQLOOM_VALUE_REDACTION_TEST", std::string{kEnvValue}.c_str(), 1);
#endif
    RunSetup setup;
    setup.routes = [](RoutedHttpClient& http) {
        http.route("/items", 400, R"({"error":"bad env-value-31337-abc"})");
    };
    const auto out = runProject(kEnvYaml, "item.read", setup);
    ASSERT_TRUE(out.result.has_value()) << out.result.error().detail;
    EXPECT_EQ(sentHeader(out, "X-Env"), kEnvValue);
    EXPECT_FALSE(contains(out.everything(), kEnvValue)) << out.everything();
}

TEST(ValueRedaction, extracted_login_token_under_plain_name_never_reaches_events) {
    constexpr const char* kLoginYaml = R"YAML(
version: 1
name: TokenRedaction
default_environment: local
environment:
  baseUrl: http://placeholder
resources:
  session:
    operations:
      login:
        method: POST
        path: /login
        body: { user: "u" }
        expect_status: 200
        extract:
          jwt: $.access_token
  item:
    operations:
      read:
        method: GET
        path: /items
        headers:
          X-Session: "{{session.jwt}}"
        expect_status: 200
)YAML";
    constexpr std::string_view kToken = "eyJhbGciOi.payload-part.sig-1234";
    RunSetup setup;
    setup.routes = [](RoutedHttpClient& http) {
        http.route("/login", 200, R"({"access_token":"eyJhbGciOi.payload-part.sig-1234"})");
        http.route("/items", 403, R"({"error":"session eyJhbGciOi.payload-part.sig-1234 denied"})");
    };
    const auto out = runProject(kLoginYaml, "item.read", setup);
    ASSERT_TRUE(out.result.has_value()) << out.result.error().detail;
    EXPECT_EQ(sentHeader(out, "X-Session"), kToken) << "the request sent must be unchanged";
    EXPECT_FALSE(contains(out.everything(), kToken)) << out.everything();
    const auto* failed = out.failedStep();
    ASSERT_NE(failed, nullptr);
    EXPECT_TRUE(contains(failed->detail, "denied")) << failed->detail;
    EXPECT_FALSE(contains(failed->detail, kToken)) << failed->detail;

    // With body capture on, the login response itself is an explicit opt-in
    // capture emitted before the token is extracted; every event after the
    // extraction is clean.
    setup.options.captureResponseBodies = true;
    const auto captured = runProject(kLoginYaml, "item.read", setup);
    ASSERT_TRUE(captured.result.has_value());

    std::string afterExtraction{};
    bool seenExtraction{false};
    for (const auto& e : captured.emitted) {
        if (std::holds_alternative<ce::ExtractionCompleted>(e)) {
            seenExtraction = true;
        }
        if (seenExtraction) {
            afterExtraction += eventText(e) + "\n";
        }
    }
    ASSERT_TRUE(seenExtraction);
    EXPECT_FALSE(contains(afterExtraction, kToken)) << afterExtraction;
}

TEST(ValueRedaction, captured_login_body_reaches_events_and_history_as_received) {
    constexpr const char* kLoginYaml = R"YAML(
version: 1
name: CapturedLogin
default_environment: local
environment:
  baseUrl: http://placeholder
resources:
  session:
    operations:
      login:
        method: POST
        path: /login
        body: { user: "x", password: "hunter2hunter2" }
        expect_status: 200
        extract:
          jwt: $.access_token
)YAML";
    RunSetup setup;
    setup.routes = [](RoutedHttpClient& http) {
        http.route("/login", 200, R"({"access_token":"abc123def456","user":"x"})");
    };
    setup.options.captureResponseBodies = true;

    const auto out = runProject(kLoginYaml, "session.login", setup);

    ASSERT_TRUE(out.result.has_value()) << out.result.error().detail;
    ASSERT_FALSE(out.sent.empty());
    EXPECT_TRUE(contains(out.sent.front().body.value_or(""), "hunter2hunter2"))
        << "the request sent must be unchanged";
    const auto bodies = out.capturedBodies();
    ASSERT_EQ(bodies.size(), 2U);
    for (const auto& body : bodies) {
        EXPECT_EQ(body, R"({"access_token":"abc123def456","user":"x"})");
    }
    const auto all = out.everything();
    EXPECT_FALSE(contains(all, "abc123def456")) << all;
    EXPECT_FALSE(contains(all, "hunter2hunter2")) << all;
}

TEST(ValueRedaction, actor_session_variable_under_plain_name_is_redacted) {
    constexpr const char* kActorYaml = R"YAML(
version: 1
name: ActorRedaction
default_environment: local
environment:
  baseUrl: http://placeholder
actors:
  user:
    auth:
      strategy: simple
      method: POST
      path: /auth/login
      body: { email: "u@x.test" }
      expect_status: 200
      extract:
        sid: $.data.sid
    inject:
      headers:
        X-Sid: "{{user.sid}}"
resources:
  item:
    operations:
      read:
        method: GET
        path: /items
        actor: user
        expect_status: 200
)YAML";
    constexpr std::string_view kSid = "sid-7a7a7a7a7a7a";
    RunSetup setup;
    setup.routes = [](RoutedHttpClient& http) {
        http.route("/auth/login", 200, R"({"data":{"sid":"sid-7a7a7a7a7a7a"}})");
        http.route("/items", 400, R"({"error":"sid sid-7a7a7a7a7a7a expired"})");
    };
    const auto out = runProject(kActorYaml, "item.read", setup);
    ASSERT_TRUE(out.result.has_value()) << out.result.error().detail;
    EXPECT_EQ(sentHeader(out, "X-Sid"), kSid);
    EXPECT_FALSE(contains(out.everything(), kSid)) << out.everything();
    const auto* failed = out.failedStep();
    ASSERT_NE(failed, nullptr);
    EXPECT_TRUE(contains(failed->detail, "expired")) << failed->detail;
}

TEST(ValueRedaction, failed_step_detail_shows_api_error_text_without_capture) {
    RunSetup setup;
    setup.routes = [](RoutedHttpClient& http) {
        http.route("/items", 422, R"({"error":"quota exceeded for plan"})");
    };
    constexpr const char* kPlainYaml = R"YAML(
version: 1
name: PlainFailure
default_environment: local
environment:
  baseUrl: http://placeholder
resources:
  item:
    operations:
      read:
        method: GET
        path: /items
        expect_status: 200
)YAML";
    const auto out = runProject(kPlainYaml, "item.read", setup);
    ASSERT_TRUE(out.result.has_value());
    const auto* failed = out.failedStep();
    ASSERT_NE(failed, nullptr);
    EXPECT_TRUE(contains(failed->detail, "HTTP 422")) << failed->detail;
    EXPECT_TRUE(contains(failed->detail, "quota exceeded for plan")) << failed->detail;
}

TEST(ValueRedaction, failed_login_detail_shows_error_text_but_not_the_token) {
    constexpr const char* kLoginYaml = R"YAML(
version: 1
name: LoginFailure
default_environment: local
environment:
  baseUrl: http://placeholder
resources:
  session:
    operations:
      login:
        method: POST
        path: /auth/login
        body: { user: "u", password: "p" }
        expect_status: 200
        extract:
          access_token: $.access_token
)YAML";
    RunSetup setup;
    setup.routes = [](RoutedHttpClient& http) {
        http.route("/auth/login", 401, R"({"error":"mfa required","access_token":"abc123def456"})");
    };
    const auto out = runProject(kLoginYaml, "session.login", setup);
    ASSERT_TRUE(out.result.has_value());
    const auto* failed = out.failedStep();
    ASSERT_NE(failed, nullptr);
    EXPECT_TRUE(contains(failed->detail, "mfa required")) << failed->detail;
    EXPECT_FALSE(contains(failed->detail, "abc123def456")) << failed->detail;
    EXPECT_FALSE(contains(out.everything(), "abc123def456")) << out.everything();
}

TEST(ValueRedaction, inline_oauth2_token_endpoint_error_is_masked) {
    constexpr const char* kOAuthYaml = R"YAML(
version: 1
name: OAuthFailure
default_environment: local
environment:
  baseUrl: http://placeholder
resources:
  item:
    operations:
      read:
        method: GET
        path: /items
        expect_status: 200
        auth:
          type: oauth2
          token_url: http://placeholder/token
          client_id: cid
          client_secret: client-secret-value-77
)YAML";
    RunSetup setup;
    setup.routes = [](RoutedHttpClient& http) {
        http.route("/token",
                   400,
                   R"({"error":"invalid_client","detail":"client-secret-value-77 rejected",)"
                   R"("access_token":"leaky-token-123"})");
    };
    const auto out = runProject(kOAuthYaml, "item.read", setup);
    ASSERT_TRUE(out.result.has_value()) << out.result.error().detail;
    const auto* failed = out.failedStep();
    ASSERT_NE(failed, nullptr);
    EXPECT_TRUE(contains(failed->detail, "invalid_client")) << failed->detail;
    EXPECT_FALSE(contains(failed->detail, "leaky-token-123")) << failed->detail;
    EXPECT_FALSE(contains(out.everything(), "client-secret-value-77")) << out.everything();
}
