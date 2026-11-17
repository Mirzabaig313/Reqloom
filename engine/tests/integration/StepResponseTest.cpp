// StepResponseTest — `RunOptions::recordResponses` puts what the API returned on each StepResult
// (status, body, extracted values) for agent-facing output (PRD FR-13.11a), masked with the run's
// final secret registry and only then cut (FR-15.3, NFR-3.7). A token extracted under an ordinary
// name and later sent as a credential is masked where it was extracted too. All values are fake.
#include "infrastructure/hooks/HookRunner.h"
#include "infrastructure/http/HttpClient.h"
#include "infrastructure/schema/SchemaParser.h"
#include "infrastructure/secrets/SecretStore.h"
#include "infrastructure/storage/HistoryStore.h"

#include <reqloom/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ce = reqloom::engine;

namespace {

/// A fake session id longer than the 256-byte extraction trace, so a cut copy would leak a prefix.
[[nodiscard]] std::string fakeSessionId() {
    std::string id{"fake-sid-"};
    while (id.size() < 300) {
        id += "0123456789abcdef";
    }
    return id;
}

/// Answers by path from a fixed table.
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
    std::vector<std::pair<std::string, ce::HttpResponse>> routes;
    std::vector<ce::HttpRequest> requests;
};

[[nodiscard]] ce::HttpResponse respond(const int status, std::string body) {
    ce::HttpResponse response;
    response.status = status;
    response.body = std::move(body);
    return response;
}

/// `session.open` extracts `sid` from `$.data.sid` (ordinary names); `order.pay` sends it as a
/// bearer token and answers 422 with a body quoting it after `pad` bytes of filler.
struct Fixture {
    ScriptedHttpClient* http{nullptr};
    ce::ExecutionEngine engine;
    ce::Project project;
};

[[nodiscard]] ce::Project buildProject() {
    ce::Project project;
    project.name = "StepResponse";
    project.defaultEnvironment = "local";
    project.environments["local"] = {{"baseUrl", "http://sut.test"}};

    ce::Operation open;
    open.id = ce::OperationId{"session.open"};
    open.resource = ce::ResourceId{"session"};
    open.method = ce::HttpMethod::Post;
    open.pathTemplate = "/sessions";
    open.expectStatus = 201;
    ce::Extraction sid;
    sid.variableName = "sid";
    sid.sourcePath = "$.data.sid";
    open.extractions.push_back(sid);
    ce::Resource session;
    session.id = ce::ResourceId{"session"};
    session.operations["open"] = std::move(open);
    project.resources[session.id] = std::move(session);

    ce::Operation pay;
    pay.id = ce::OperationId{"order.pay"};
    pay.resource = ce::ResourceId{"order"};
    pay.method = ce::HttpMethod::Post;
    pay.pathTemplate = "/pay";
    pay.headers["Authorization"] = "Bearer {{session.sid}}";
    pay.expectStatus = 200;
    ce::Resource order;
    order.id = ce::ResourceId{"order"};
    order.operations["pay"] = std::move(pay);
    project.resources[order.id] = std::move(order);
    return project;
}

[[nodiscard]] std::unique_ptr<Fixture> makeFixture(const std::size_t pad) {
    auto http = std::make_unique<ScriptedHttpClient>();
    const auto token = fakeSessionId();
    http->routes.emplace_back(
        "/sessions", respond(201, R"({"data":{"sid":")" + token + R"(","note":"welcome"}})"));
    http->routes.emplace_back(
        "/pay",
        respond(422, std::string(pad, 'x') + " session " + token + " cannot pay; card declined"));
    auto* raw = http.get();
    ce::ExecutionEngine::Dependencies deps{std::move(http), nullptr, nullptr, nullptr, nullptr};
    return std::make_unique<Fixture>(raw, ce::ExecutionEngine{std::move(deps)}, buildProject());
}

[[nodiscard]] const ce::StepResult& row(const ce::RunResult& result, const std::string& op) {
    for (const auto& step : result.steps) {
        if (step.op.value == op) {
            return step;
        }
    }
    throw std::runtime_error("no row for " + op);
}

/// True when any 12-byte piece of the fake token appears in `text`.
[[nodiscard]] bool leaksToken(const std::string& text) {
    const auto token = fakeSessionId();
    constexpr std::size_t kPiece = 12;
    for (std::size_t i = 9; i + kPiece <= token.size(); i += 4) {
        if (text.find(token.substr(i, kPiece)) != std::string::npos) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] ce::RunOptions recording(const std::size_t bodyBytes) {
    ce::RunOptions options;
    options.recordResponses = true;
    options.agentOutput = true;
    options.responseBodyBytes = bodyBytes;
    options.failureExcerptBytes = 0;
    return options;
}

}  // namespace

TEST(StepResponse, status_body_and_extractions_are_recorded_per_step) {
    auto f = makeFixture(0);
    ce::RunContext ctx;
    const auto result =
        f->engine.run(f->project, ce::OperationId{"order.pay"}, ctx, recording(4096));
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    const auto& open = row(*result, "session.open");
    EXPECT_EQ(open.httpStatus, 201);
    ASSERT_TRUE(open.response.has_value());
    EXPECT_NE(open.response->body.find("welcome"), std::string::npos) << open.response->body;
    EXPECT_FALSE(open.response->truncated);
    EXPECT_EQ(open.response->limitBytes, 4096U);
    ASSERT_EQ(open.extracted.size(), 1U);
    EXPECT_EQ(open.extracted.front().name, "sid");

    const auto& pay = row(*result, "order.pay");
    EXPECT_EQ(pay.httpStatus, 422);
    ASSERT_TRUE(pay.response.has_value());
    EXPECT_NE(pay.response->body.find("card declined"), std::string::npos) << pay.response->body;
}

TEST(StepResponse, nothing_is_recorded_unless_asked) {
    auto f = makeFixture(0);
    ce::RunContext ctx;
    const auto result = f->engine.run(f->project, ce::OperationId{"order.pay"}, ctx);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    for (const auto& step : result->steps) {
        EXPECT_FALSE(step.response.has_value()) << step.op.value;
        EXPECT_TRUE(step.extracted.empty()) << step.op.value;
    }
    EXPECT_EQ(row(*result, "order.pay").httpStatus, 422) << "the status is always recorded";
}

TEST(StepResponse, token_used_later_as_a_credential_is_masked_where_it_was_extracted) {
    auto f = makeFixture(0);
    ce::RunContext ctx;
    const auto result =
        f->engine.run(f->project, ce::OperationId{"order.pay"}, ctx, recording(4096));
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    const auto& open = row(*result, "session.open");
    ASSERT_TRUE(open.response.has_value());
    EXPECT_FALSE(leaksToken(open.response->body)) << open.response->body;
    EXPECT_NE(open.response->body.find("<redacted>"), std::string::npos) << open.response->body;
    ASSERT_EQ(open.extracted.size(), 1U);
    EXPECT_FALSE(leaksToken(open.extracted.front().value)) << open.extracted.front().value;

    const auto& pay = row(*result, "order.pay");
    ASSERT_TRUE(pay.response.has_value());
    EXPECT_FALSE(leaksToken(pay.response->body)) << pay.response->body;
    EXPECT_FALSE(leaksToken(pay.detail)) << pay.detail;
}

TEST(StepResponse, body_is_masked_before_it_is_cut) {
    // The cut falls inside the token: a body cut first would keep a prefix nothing matches.
    constexpr std::size_t kPad = 100;
    auto f = makeFixture(kPad);
    ce::RunContext ctx;
    const auto result =
        f->engine.run(f->project, ce::OperationId{"order.pay"}, ctx, recording(200));
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    const auto& pay = row(*result, "order.pay");
    ASSERT_TRUE(pay.response.has_value());
    EXPECT_FALSE(leaksToken(pay.response->body)) << pay.response->body;
    EXPECT_LE(pay.response->body.size(), 200U);
    EXPECT_FALSE(pay.response->truncated) << "masked, the body fits: " << pay.response->body;

    auto g = makeFixture(kPad);
    ce::RunContext ctx2;
    const auto cut = g->engine.run(g->project, ce::OperationId{"order.pay"}, ctx2, recording(140));
    ASSERT_TRUE(cut.has_value()) << cut.error().detail;
    const auto& cutPay = row(*cut, "order.pay");
    ASSERT_TRUE(cutPay.response.has_value());
    EXPECT_TRUE(cutPay.response->truncated);
    EXPECT_EQ(cutPay.response->limitBytes, 140U);
    EXPECT_GT(cutPay.response->totalBytes, 300U);
    EXPECT_LE(cutPay.response->body.size(), 140U);
    EXPECT_FALSE(leaksToken(cutPay.response->body)) << cutPay.response->body;
}

TEST(StepResponse, zero_body_bytes_keeps_status_and_extractions_without_a_body) {
    auto f = makeFixture(0);
    ce::RunContext ctx;
    const auto result = f->engine.run(f->project, ce::OperationId{"order.pay"}, ctx, recording(0));
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    const auto& open = row(*result, "session.open");
    ASSERT_TRUE(open.response.has_value());
    EXPECT_TRUE(open.response->body.empty());
    EXPECT_TRUE(open.response->truncated);
    EXPECT_EQ(open.extracted.size(), 1U);
}

TEST(StepResponse, secret_looking_extraction_is_always_the_marker) {
    auto f = makeFixture(0);
    auto& open = f->project.resources.at(ce::ResourceId{"session"}).operations.at("open");
    open.extractions.front().variableName = "access_token";
    auto& pay = f->project.resources.at(ce::ResourceId{"order"}).operations.at("pay");
    pay.headers["Authorization"] = "Bearer {{session.access_token}}";
    ce::RunContext ctx;
    const auto result =
        f->engine.run(f->project, ce::OperationId{"order.pay"}, ctx, recording(4096));
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    const auto& opened = row(*result, "session.open");
    ASSERT_EQ(opened.extracted.size(), 1U);
    EXPECT_EQ(opened.extracted.front().value, "<redacted>");
}

TEST(StepResponse, credential_in_a_sensitive_query_parameter_is_masked_too) {
    auto f = makeFixture(0);
    auto& pay = f->project.resources.at(ce::ResourceId{"order"}).operations.at("pay");
    pay.headers.clear();
    pay.queryParams["api_key"] = "{{session.sid}}";
    ce::RunContext ctx;
    const auto result =
        f->engine.run(f->project, ce::OperationId{"order.pay"}, ctx, recording(4096));
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    const auto& open = row(*result, "session.open");
    ASSERT_TRUE(open.response.has_value());
    EXPECT_FALSE(leaksToken(open.response->body)) << open.response->body;
}

TEST(StepResponse, cookie_value_sent_in_a_cookie_header_is_masked) {
    auto f = makeFixture(0);
    auto& pay = f->project.resources.at(ce::ResourceId{"order"}).operations.at("pay");
    pay.headers.clear();
    pay.headers["Cookie"] = "theme=dark; sid={{session.sid}}";
    ce::RunContext ctx;
    const auto result =
        f->engine.run(f->project, ce::OperationId{"order.pay"}, ctx, recording(4096));
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    const auto& open = row(*result, "session.open");
    ASSERT_TRUE(open.response.has_value());
    EXPECT_FALSE(leaksToken(open.response->body)) << open.response->body;
}

TEST(StepResponse, credential_extraction_is_masked_even_when_the_run_stops_before_sending_it) {
    // order.check fails, so order.pay (the only use of sid, as a bearer token) never runs: the
    // project, not the wire, says sid is a credential.
    auto f = makeFixture(0);
    ce::Operation check;
    check.id = ce::OperationId{"order.check"};
    check.resource = ce::ResourceId{"order"};
    check.method = ce::HttpMethod::Post;
    check.pathTemplate = "/check";
    check.expectStatus = 200;
    check.explicitDependencies.push_back(ce::OperationId{"session.open"});
    auto& order = f->project.resources.at(ce::ResourceId{"order"});
    order.operations["check"] = std::move(check);
    order.operations.at("pay").explicitDependencies.push_back(ce::OperationId{"order.check"});
    f->http->routes.emplace_back("/check", respond(500, R"({"error":"down"})"));
    ce::RunContext ctx;
    const auto result =
        f->engine.run(f->project, ce::OperationId{"order.pay"}, ctx, recording(4096));
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_EQ(row(*result, "order.pay").status, ce::StepResult::Status::Blocked);

    const auto& open = row(*result, "session.open");
    ASSERT_TRUE(open.response.has_value());
    EXPECT_FALSE(leaksToken(open.response->body)) << open.response->body;
    ASSERT_EQ(open.extracted.size(), 1U);
    EXPECT_EQ(open.extracted.front().value, "<redacted>");
}

TEST(StepResponse, ordinary_values_sent_in_ordinary_parameters_stay_readable) {
    // `keyword`, `author` and a `sort_key` body field are not credentials: their values stay.
    auto f = makeFixture(0);
    auto& open = f->project.resources.at(ce::ResourceId{"session"}).operations.at("open");
    ce::Extraction note;
    note.variableName = "note";
    note.sourcePath = "$.data.note";
    open.extractions.push_back(note);
    auto& pay = f->project.resources.at(ce::ResourceId{"order"}).operations.at("pay");
    pay.queryParams["keyword"] = "{{session.note}}";
    pay.queryParams["author"] = "{{session.note}}";
    pay.bodyTemplate = R"({"sort_key":"{{session.note}}"})";
    ce::RunContext ctx;
    const auto result =
        f->engine.run(f->project, ce::OperationId{"order.pay"}, ctx, recording(4096));
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    const auto& opened = row(*result, "session.open");
    ASSERT_TRUE(opened.response.has_value());
    EXPECT_NE(opened.response->body.find("welcome"), std::string::npos) << opened.response->body;
    ASSERT_EQ(opened.extracted.size(), 2U);
    EXPECT_EQ(opened.extracted.back().value, "welcome");
}

TEST(StepResponse, object_extraction_masks_credential_fields_inside_it) {
    auto f = makeFixture(0);
    f->http->routes.front().second.body =
        R"({"data":{"sid":"s1","note":"welcome","access_token":"fake-at-object-0001"}})";
    auto& open = f->project.resources.at(ce::ResourceId{"session"}).operations.at("open");
    open.extractions.front().variableName = "payload";
    open.extractions.front().sourcePath = "$.data";
    auto& pay = f->project.resources.at(ce::ResourceId{"order"}).operations.at("pay");
    pay.headers.clear();
    pay.pathTemplate = "/pay?x={{session.payload}}";
    ce::RunContext ctx;
    const auto result =
        f->engine.run(f->project, ce::OperationId{"order.pay"}, ctx, recording(4096));
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    const auto& opened = row(*result, "session.open");
    ASSERT_EQ(opened.extracted.size(), 1U);
    EXPECT_EQ(opened.extracted.front().value.find("fake-at-object-0001"), std::string::npos)
        << opened.extracted.front().value;
}

TEST(StepResponse, deeply_nested_body_is_masked_without_recursion) {
    auto f = makeFixture(0);
    // Valid JSON nested 50,000 deep: a recursive walk would overflow the stack.
    f->http->routes.front().second.body = std::string(50000, '[') + std::string(50000, ']');
    ce::RunContext ctx;
    const auto result = f->engine.run(f->project, ce::OperationId{"order.pay"}, ctx, recording(64));
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    const auto& open = row(*result, "session.open");
    ASSERT_TRUE(open.response.has_value());
    EXPECT_EQ(open.response->totalBytes, 100000U);
}
