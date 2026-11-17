// AgentMaskingTest — what `RunOptions::agentOutput` (the CLI) masks beyond the desktop, and proof
// that without it the desktop sees exactly what it saw before. All values are fake.
#include "application/StepEvidence.h"
#include "domain/SecretRegistry.h"
#include "infrastructure/hooks/HookRunner.h"
#include "infrastructure/http/HttpClient.h"
#include "infrastructure/schema/SchemaParser.h"
#include "infrastructure/secrets/SecretStore.h"
#include "infrastructure/storage/HistoryStore.h"
#include "support/HermeticDependencies.h"

#include <reqloom/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace ce = reqloom::engine;

namespace {

class ScopedEnv {
public:
    ScopedEnv(std::string name, const std::string& value) : name_(std::move(name)) {
#ifdef _WIN32
        _putenv_s(name_.c_str(), value.c_str());
#else
        setenv(name_.c_str(), value.c_str(), 1);
#endif
    }
    ~ScopedEnv() {
#ifdef _WIN32
        _putenv_s(name_.c_str(), "");
#else
        unsetenv(name_.c_str());
#endif
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;
    ScopedEnv(ScopedEnv&&) = delete;
    ScopedEnv& operator=(ScopedEnv&&) = delete;

private:
    std::string name_;
};

[[nodiscard]] std::string fakeToken(const std::string& stem) {
    std::string token{stem};
    while (token.size() < 120) {
        token += "0123456789abcdef";
    }
    return token;
}

/// Answers by path suffix from a fixed table.
class ScriptedHttpClient final : public ce::HttpClient {
public:
    std::expected<ce::HttpResponse, ce::ReqloomError> send(const ce::HttpRequest& req) override {
        requests.push_back(req);
        for (const auto& [suffix, response] : routes) {
            if (req.url.ends_with(suffix)) {
                return response;
            }
        }
        ce::HttpResponse notFound;
        notFound.status = 404;
        return notFound;
    }
    std::map<std::string, ce::HttpResponse> routes;
    std::vector<ce::HttpRequest> requests;
};

[[nodiscard]] ce::HttpResponse respond(
    const int status,
    std::string body,
    std::vector<std::pair<std::string, std::string>> headers = {}) {
    ce::HttpResponse response;
    response.status = status;
    response.body = std::move(body);
    response.headers = std::move(headers);
    return response;
}

[[nodiscard]] ce::Operation operation(const std::string& resource,
                                      const std::string& name,
                                      const std::string& path,
                                      const int expect) {
    ce::Operation op;
    op.id = ce::OperationId{resource + "." + name};
    op.resource = ce::ResourceId{resource};
    op.method = ce::HttpMethod::Post;
    op.pathTemplate = path;
    op.expectStatus = expect;
    return op;
}

void addOperation(ce::Project& project, ce::Operation op) {
    auto& resource = project.resources[op.resource];
    resource.id = op.resource;
    const auto name = op.id.value.substr(op.id.value.find('.') + 1);
    resource.operations[name] = std::move(op);
}

void extract(ce::Operation& op, const std::string& variable, const std::string& path) {
    ce::Extraction e;
    e.variableName = variable;
    e.sourcePath = path;
    op.extractions.push_back(e);
}

[[nodiscard]] ce::Project emptyProject() {
    ce::Project project;
    project.name = "AgentMasking";
    project.defaultEnvironment = "local";
    project.environments["local"] = {{"baseUrl", "http://sut.test"}};
    return project;
}

struct Harness {
    ScriptedHttpClient* http{nullptr};
    std::unique_ptr<ce::ExecutionEngine> engine;
    std::vector<ce::RunEvent> events;
};

[[nodiscard]] std::unique_ptr<Harness> makeHarness() {
    auto harness = std::make_unique<Harness>();
    auto http = std::make_unique<ScriptedHttpClient>();
    harness->http = http.get();
    ce::ExecutionEngine::Dependencies deps{std::move(http), nullptr, nullptr, nullptr, nullptr};
    deps.hostApprovals = std::make_unique<reqloom::tests::MemoryHostApprovals>(true);
    harness->engine = std::make_unique<ce::ExecutionEngine>(std::move(deps));
    auto* raw = harness.get();
    harness->engine->subscribe([raw](const ce::RunEvent& event) { raw->events.push_back(event); });
    return harness;
}

[[nodiscard]] ce::RunOptions cli() {
    ce::RunOptions options;
    options.agentOutput = true;
    options.recordResponses = true;
    options.secretsFromEnvironment = true;
    options.responseBodyBytes = 8192;
    return options;
}

[[nodiscard]] const ce::StepResult& row(const ce::RunResult& result, const std::string& op) {
    for (const auto& step : result.steps) {
        if (step.op.value == op && !step.pollAttempt && !step.forEachIndex) {
            return step;
        }
    }
    throw std::runtime_error("no row for " + op);
}

[[nodiscard]] std::string extracted(const ce::StepResult& step, const std::string& name) {
    for (const auto& value : step.extracted) {
        if (value.name == name) {
            return value.value;
        }
    }
    return "<missing>";
}

[[nodiscard]] std::string bodyOf(const ce::StepResult& step) {
    return step.response ? step.response->body : std::string{"<no response>"};
}

/// `session.open` returns `token` under `$.data.sid`; `order.pay` sends it as a bearer token and
/// answers 422 quoting it.
[[nodiscard]] ce::Project bearerProject() {
    auto project = emptyProject();
    auto open = operation("session", "open", "/sessions", 201);
    extract(open, "sid", "$.data.sid");
    addOperation(project, std::move(open));
    auto pay = operation("order", "pay", "/pay", 200);
    pay.headers["Authorization"] = "Bearer {{session.sid}}";
    addOperation(project, std::move(pay));
    return project;
}

}  // namespace

// ─── Owner decision: the desktop shows what it showed before ─────────────────

TEST(AgentMasking, desktop_run_shows_values_exactly_as_before) {
    const auto token = fakeToken("fake-desktop-sid-");
    auto h = makeHarness();
    h->http->routes["/sessions"] = respond(201, R"({"data":{"sid":")" + token + R"("}})");
    h->http->routes["/pay"] = respond(422, R"({"error":"session )" + token + R"( declined"})");
    auto project = bearerProject();
    ce::RunOptions desktop;
    desktop.captureResponseBodies = true;
    ce::RunContext ctx;
    const auto result = h->engine->run(project, ce::OperationId{"order.pay"}, ctx, desktop);
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    bool sawExtraction = false;
    bool sawMaskedAuthorization = false;
    for (const auto& event : h->events) {
        if (const auto* e = std::get_if<ce::ExtractionCompleted>(&event)) {
            EXPECT_EQ(e->value, token) << "an ordinary extraction is shown as extracted";
            sawExtraction = true;
        }
        if (const auto* p = std::get_if<ce::RequestPrepared>(&event)) {
            for (const auto& [name, value] : p->maskedHeaders) {
                if (name == "Authorization") {
                    EXPECT_EQ(value, "<redacted>") << "masking by header name stays";
                    sawMaskedAuthorization = true;
                }
            }
        }
        if (const auto* r = std::get_if<ce::ResponseReceived>(&event)) {
            ASSERT_TRUE(r->body.has_value());
            EXPECT_NE(r->body->find(token), std::string::npos) << "captured bodies as received";
        }
        if (const auto* f = std::get_if<ce::StepFailed>(&event)) {
            EXPECT_NE(f->detail.find(token), std::string::npos) << f->detail;
        }
    }
    EXPECT_TRUE(sawExtraction);
    EXPECT_TRUE(sawMaskedAuthorization);
    EXPECT_NE(row(*result, "order.pay").detail.find(token), std::string::npos);
    EXPECT_EQ(ctx.secretRegistry().redact(token), token) << "nothing new was registered";
}

// ─── Finding 1: credential-like field names in bodies and extractions ───────

TEST(AgentMasking, jwt_and_session_id_fields_are_masked_even_when_never_sent) {
    auto h = makeHarness();
    h->http->routes["/login"] =
        respond(200,
                R"({"jwt":"fake-jwt-0123456789","session_id":"fake-session-0123456789",)"
                R"("user":"ada"})");
    auto project = emptyProject();
    auto login = operation("auth", "login", "/login", 200);
    extract(login, "jwt", "$.jwt");
    extract(login, "session_id", "$.session_id");
    addOperation(project, std::move(login));
    ce::RunContext ctx;
    const auto result = h->engine->run(project, ce::OperationId{"auth.login"}, ctx, cli());
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    const auto& step = row(*result, "auth.login");
    EXPECT_EQ(bodyOf(step).find("fake-jwt"), std::string::npos) << bodyOf(step);
    EXPECT_EQ(bodyOf(step).find("fake-session"), std::string::npos) << bodyOf(step);
    EXPECT_NE(bodyOf(step).find("ada"), std::string::npos) << bodyOf(step);
    EXPECT_EQ(extracted(step, "jwt"), "<redacted>");
    EXPECT_EQ(extracted(step, "session_id"), "<redacted>");
}

// ─── Finding 2: credential headers the header policy already masks ──────────

TEST(AgentMasking, extraction_feeding_any_masked_header_is_masked_where_it_came_from) {
    for (const std::string header : {"X-Auth-Key",
                                     "Ocp-Apim-Subscription-Key",
                                     "X-Functions-Key",
                                     "Authentication",
                                     "X-Auth",
                                     "auth_key"}) {
        const auto token = fakeToken("fake-header-value-");
        auto h = makeHarness();
        h->http->routes["/sessions"] =
            respond(201, R"({"data":{"sid":")" + token + R"(","note":"welcome"}})");
        h->http->routes["/pay"] = respond(500, R"({"error":"down"})");
        auto project = bearerProject();
        auto& pay = project.resources.at(ce::ResourceId{"order"}).operations.at("pay");
        pay.headers.clear();
        pay.headers[header] = "{{session.sid}}";
        ce::RunContext ctx;
        const auto result = h->engine->run(project, ce::OperationId{"order.pay"}, ctx, cli());
        ASSERT_TRUE(result.has_value()) << result.error().detail;
        const auto& open = row(*result, "session.open");
        EXPECT_EQ(bodyOf(open).find(token), std::string::npos) << header << ": " << bodyOf(open);
        EXPECT_EQ(extracted(open, "sid"), "<redacted>") << header;
    }
}

// ─── Finding 3: secrets with characters JSON escapes ─────────────────────────

TEST(AgentMasking, secret_with_quote_and_backslash_is_masked_after_json_reserialisation) {
    const std::string secret = R"(p@ss\w"rd9)";
    const ScopedEnv env{"REQLOOM_SECRET_PASS", secret};
    auto h = makeHarness();
    // The API echoes the secret inside JSON, so it arrives escaped: p@ss\\w\"rd9.
    h->http->routes["/sessions"] =
        respond(201, R"({"data":{"echo":"pw p@ss\\w\"rd9 seen","sid":"s"}})");
    h->http->routes["/pay"] = respond(422, R"({"error":"bad password p@ss\\w\"rd9"})");
    auto project = bearerProject();
    auto& open = project.resources.at(ce::ResourceId{"session"}).operations.at("open");
    extract(open, "payload", "$.data");
    auto& pay = project.resources.at(ce::ResourceId{"order"}).operations.at("pay");
    pay.headers["X-Pass"] = "{{secret.PASS}}";
    ce::RunContext ctx;
    const auto result = h->engine->run(project, ce::OperationId{"order.pay"}, ctx, cli());
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    const auto& opened = row(*result, "session.open");
    const auto& paid = row(*result, "order.pay");
    for (const auto& text :
         {bodyOf(opened), extracted(opened, "payload"), bodyOf(paid), paid.detail}) {
        EXPECT_EQ(text.find("p@ss"), std::string::npos) << text;
    }
}

// ─── Finding 4: a session cookie set by the last response ────────────────────

TEST(AgentMasking, cookie_set_by_a_response_is_masked_even_if_never_sent_back) {
    const auto token = fakeToken("fake-cookie-sid-");
    auto h = makeHarness();
    h->http->routes["/sessions"] = respond(201,
                                           R"({"data":{"welcome":")" + token + R"("}})",
                                           {{"Set-Cookie", "sid=" + token + "; Path=/; HttpOnly"}});
    auto project = emptyProject();
    addOperation(project, operation("session", "open", "/sessions", 201));
    ce::RunContext ctx;
    const auto result = h->engine->run(project, ce::OperationId{"session.open"}, ctx, cli());
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    const auto& open = row(*result, "session.open");
    EXPECT_EQ(bodyOf(open).find(token), std::string::npos) << bodyOf(open);
}

// ─── Finding 6: what a row keeps in memory ───────────────────────────────────

TEST(AgentMasking, a_row_keeps_only_what_its_limit_needs_and_nothing_at_zero) {
    const std::string body(3 * 1024 * 1024, 'x');
    ce::StepResult kept;
    ce::keepResponseBody(kept, body, 2048);
    ASSERT_TRUE(kept.response.has_value());
    EXPECT_EQ(kept.response->totalBytes, body.size());
    EXPECT_LT(kept.response->body.size(), std::size_t{128} * 1024);

    ce::StepResult none;
    ce::keepResponseBody(none, body, 0);
    ASSERT_TRUE(none.response.has_value());
    EXPECT_TRUE(none.response->body.empty());
    EXPECT_EQ(none.response->totalBytes, body.size());
}

TEST(AgentMasking, a_secret_cut_by_the_retention_limit_leaves_no_prefix) {
    // A token longer than the retention margin: the kept part ends inside it.
    std::string token{"fake-huge-token-"};
    while (token.size() < 100 * 1024) {
        token += "0123456789abcdef";
    }
    ce::RunContext ctx;
    ctx.secretRegistry().add(token);
    std::vector<ce::StepResult> rows(1);
    ce::keepResponseBody(rows[0], token, 100);
    ce::finalizeStepEvidence(rows, ctx, 100);
    ASSERT_TRUE(rows[0].response.has_value());
    EXPECT_EQ(rows[0].response->body.find("fake-huge"), std::string::npos)
        << rows[0].response->body;
    EXPECT_TRUE(rows[0].response->truncated);
}

// ─── Finding 7: hostile request body nesting ─────────────────────────────────

TEST(AgentMasking, deeply_nested_request_body_does_not_overflow_the_stack) {
    auto h = makeHarness();
    h->http->routes["/sessions"] = respond(201, R"({"ok":true})");
    auto project = emptyProject();
    auto open = operation("session", "open", "/sessions", 201);
    open.bodyTemplate = std::string(50000, '[') + std::string(50000, ']');
    addOperation(project, std::move(open));
    ce::RunContext ctx;
    const auto result = h->engine->run(project, ce::OperationId{"session.open"}, ctx, cli());
    ASSERT_TRUE(result.has_value()) << result.error().detail;
}

// ─── Finding 8: over-masking ─────────────────────────────────────────────────

TEST(AgentMasking, ordinary_words_sent_in_key_value_and_hint_fields_stay_readable) {
    auto h = makeHarness();
    h->http->routes["/sessions"] = respond(
        201, R"({"data":{"sid":"s","note":"Environment production access_token Kept-Word"}})");
    h->http->routes["/pay"] = respond(500, R"({"error":"down"})");
    auto project = bearerProject();
    auto& pay = project.resources.at(ce::ResourceId{"order"}).operations.at("pay");
    pay.headers.clear();
    pay.explicitDependencies.push_back(ce::OperationId{"session.open"});
    pay.bodyTemplate = R"({"tags":[{"Key":"Environment","Value":"production"}],)"
                       R"("token_type_hint":"access_token","auth":"Kept-Word"})";
    ce::RunContext ctx;
    const auto result = h->engine->run(project, ce::OperationId{"order.pay"}, ctx, cli());
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    const auto body = bodyOf(row(*result, "session.open"));
    for (const auto* word : {"Environment", "production", "access_token", "Kept-Word"}) {
        EXPECT_NE(body.find(word), std::string::npos) << word << ": " << body;
    }
}

TEST(AgentMasking, form_hint_field_value_stays_readable) {
    auto h = makeHarness();
    h->http->routes["/sessions"] = respond(201, R"({"data":{"sid":"s","note":"access_token"}})");
    h->http->routes["/pay"] = respond(500, R"({"error":"down"})");
    auto project = bearerProject();
    auto& pay = project.resources.at(ce::ResourceId{"order"}).operations.at("pay");
    pay.headers.clear();
    pay.explicitDependencies.push_back(ce::OperationId{"session.open"});
    pay.bodyForm = std::map<std::string, std::string>{{"token_type_hint", "access_token"}};
    ce::RunContext ctx;
    const auto result = h->engine->run(project, ce::OperationId{"order.pay"}, ctx, cli());
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    const auto body = bodyOf(row(*result, "session.open"));
    EXPECT_NE(body.find(R"("note":"access_token")"), std::string::npos) << body;
}

TEST(AgentMasking, json_key_is_replaced_only_when_it_holds_a_registered_secret) {
    // Coordinator item 2 (2026-11-17): a key that holds a secret value leaks it, so it is masked;
    // every other key stays as received.
    const ScopedEnv env{"REQLOOM_SECRET_PASS", "fake-key-name-77"};
    auto h = makeHarness();
    h->http->routes["/sessions"] =
        respond(201, R"({"fake-key-name-77":"fake-key-name-77","sid":"s","kind":"x"})");
    h->http->routes["/pay"] = respond(500, R"({"error":"down"})");
    auto project = bearerProject();
    auto& pay = project.resources.at(ce::ResourceId{"order"}).operations.at("pay");
    pay.headers["X-Pass"] = "{{secret.PASS}}";
    ce::RunContext ctx;
    const auto result = h->engine->run(project, ce::OperationId{"order.pay"}, ctx, cli());
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(bodyOf(row(*result, "session.open")),
              R"({"<redacted>":"<redacted>","sid":"s","kind":"x"})");
}

// ─── Coordinator item 2: secrets as non-string JSON scalars ─────────────────

TEST(AgentMasking, numeric_secret_returned_as_a_json_number_is_masked) {
    const ScopedEnv env{"REQLOOM_SECRET_PIN", "4815162342"};
    auto h = makeHarness();
    h->http->routes["/sessions"] =
        respond(201, R"({"data":{"sid":"s","account":4815162342,"count":12}})");
    h->http->routes["/pay"] = respond(422, R"({"echo":4815162342})");
    auto project = bearerProject();
    auto& open = project.resources.at(ce::ResourceId{"session"}).operations.at("open");
    extract(open, "account", "$.data.account");
    open.bodyTemplate = R"({"pin":"{{secret.PIN}}"})";
    ce::RunContext ctx;
    const auto result = h->engine->run(project, ce::OperationId{"order.pay"}, ctx, cli());
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    const auto& opened = row(*result, "session.open");
    EXPECT_EQ(bodyOf(opened), R"({"data":{"sid":"s","account":"<redacted>","count":12}})");
    EXPECT_EQ(extracted(opened, "account").find("4815162342"), std::string::npos);
    const auto& paid = row(*result, "order.pay");
    EXPECT_EQ(bodyOf(paid).find("4815162342"), std::string::npos) << bodyOf(paid);
    EXPECT_EQ(paid.detail.find("4815162342"), std::string::npos) << paid.detail;
}

// ─── Coordinator item 3: escaped spellings in JSON that does not parse ──────

TEST(AgentMasking, secret_escaped_as_slash_or_unicode_in_unparsable_json_is_masked) {
    const ScopedEnv slash{"REQLOOM_SECRET_PATHY", "fake/slash/secret-1"};
    const ScopedEnv uni{"REQLOOM_SECRET_UNI", "fake-unicode-secret-2"};
    auto h = makeHarness();
    // Cut off mid-document, as a body past the retention limit is: it cannot be parsed.
    h->http->routes["/sessions"] = respond(
        201,
        R"({"data":{"sid":"s"},"a":"fake\/slash\/secret-1","b":"\u0066ake-unicode-secr\u0065t-2","c":"ok","d":"\u0066ake-unicode)");
    h->http->routes["/pay"] = respond(500, R"({"error":"down"})");
    auto project = bearerProject();
    auto& pay = project.resources.at(ce::ResourceId{"order"}).operations.at("pay");
    pay.headers["X-A"] = "{{secret.PATHY}}";
    pay.headers["X-B"] = "{{secret.UNI}}";
    ce::RunContext ctx;
    const auto result = h->engine->run(project, ce::OperationId{"order.pay"}, ctx, cli());
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    const auto body = bodyOf(row(*result, "session.open"));
    EXPECT_EQ(body.find("slash\\/secret"), std::string::npos) << body;
    EXPECT_EQ(body.find("secr\\u0065t"), std::string::npos) << body;
    EXPECT_NE(body.find(R"("c":"ok")"), std::string::npos) << "ordinary values must stay: " << body;
}
