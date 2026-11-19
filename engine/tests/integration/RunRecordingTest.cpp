// RunRecordingTest — a CLI run records its masked step rows in run history for `reqloom show`
// (PRD FR-13.13, FR-16.3) and who started it (FR-15.8). What it sent is masked like what it
// received: a token learned in one step and sent as a credential in a later one is masked in every
// row, in the URL, headers and bodies. A desktop run records no rows. All values are fake.
#include "infrastructure/hooks/HookRunner.h"
#include "infrastructure/http/HttpClient.h"
#include "infrastructure/schema/SchemaParser.h"
#include "infrastructure/secrets/SecretStore.h"
#include "infrastructure/storage/HistoryStore.h"
#include "support/HermeticDependencies.h"
#include "support/TempPath.h"

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <chrono>
#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ce = reqloom::engine;
namespace fs = std::filesystem;

namespace {

[[nodiscard]] std::string fakeToken() {
    std::string token{"fake-recorded-sid-"};
    while (token.size() < 120) {
        token += "0123456789abcdef";
    }
    return token;
}

class ScriptedHttp final : public ce::HttpClient {
public:
    std::expected<ce::HttpResponse, ce::ReqloomError> send(const ce::HttpRequest& req) override {
        for (const auto& [needle, response] : routes) {
            if (req.url.find(needle) != std::string::npos) {
                return response;
            }
        }
        ce::HttpResponse notFound;
        notFound.status = 404;
        return notFound;
    }
    std::map<std::string, ce::HttpResponse> routes;
};

[[nodiscard]] ce::HttpResponse respond(int status, std::string body) {
    ce::HttpResponse r;
    r.status = status;
    r.body = std::move(body);
    r.headers = {{"Content-Type", "application/json"}};
    return r;
}

[[nodiscard]] ce::Operation operation(const std::string& resource,
                                      const std::string& name,
                                      const std::string& path,
                                      int expect) {
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

/// `session.open` returns the token as `$.data.sid`; `order.pay` sends it as a bearer, in a query
/// parameter and in its body, and gets a 422 quoting it.
[[nodiscard]] ce::Project project() {
    ce::Project p;
    p.name = "Recording";
    p.defaultEnvironment = "local";
    p.environments["local"] = {{"baseUrl", "http://sut.test"}};
    auto open = operation("session", "open", "/sessions", 201);
    open.extractions.push_back(ce::Extraction{.variableName = "sid", .sourcePath = "$.data.sid"});
    addOperation(p, std::move(open));
    auto pay = operation("order", "pay", "/pay?ref={{session.sid}}&page=1", 200);
    pay.headers["Authorization"] = "Bearer {{session.sid}}";
    pay.headers["X-Trace"] = "{{session.sid}}";
    pay.bodyTemplate = R"({"note":"{{session.sid}}","amount":5})";
    addOperation(p, std::move(pay));
    return p;
}

struct Rig {
    fs::path dir;
    std::unique_ptr<ce::ExecutionEngine> engine;
};

[[nodiscard]] Rig makeRig() {
    Rig rig;
    rig.dir = reqloom::tests::uniqueTempPath("reqloom-recording");
    fs::create_directories(rig.dir);
    auto http = std::make_unique<ScriptedHttp>();
    const auto token = fakeToken();
    http->routes["/sessions"] = respond(201, R"({"data":{"sid":")" + token + R"("}})");
    http->routes["/pay"] = respond(422, R"({"error":"session )" + token + R"( declined"})");
    ce::ExecutionEngine::Dependencies deps{
        std::move(http), nullptr, ce::makeSqliteHistoryStore(), nullptr, nullptr};
    deps.hostApprovals = std::make_unique<reqloom::tests::MemoryHostApprovals>(true);
    rig.engine = std::make_unique<ce::ExecutionEngine>(std::move(deps));
    EXPECT_TRUE(rig.engine->openHistory(rig.dir / "history.db").has_value());
    return rig;
}

[[nodiscard]] ce::RunOptions cliOptions() {
    ce::RunOptions options;
    options.agentOutput = true;
    options.recordResponses = true;
    options.responseBodyBytes = 65536;
    options.initiator = ce::RunInitiator{ce::RunInitiator::Kind::Agent, "test-agent"};
    return options;
}

[[nodiscard]] bool contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

/// Every string a recorded row can print, joined.
[[nodiscard]] std::string everything(const ce::StepResult& step) {
    std::string out = step.detail;
    if (step.response) {
        out += step.response->body;
    }
    for (const auto& e : step.extracted) {
        out += e.value;
    }
    if (step.request) {
        out += step.request->url + step.request->body.body;
        for (const auto& [name, value] : step.request->headers) {
            out += name + ": " + value;
        }
    }
    return out;
}

}  // namespace

TEST(RunRecording, cli_run_records_masked_rows_with_what_was_sent) {
    auto rig = makeRig();
    ce::RunContext ctx;
    const auto result = rig.engine->run(project(), ce::OperationId{"order.pay"}, ctx, cliOptions());
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    const auto rows = rig.engine->recordedSteps(result->runId);
    ASSERT_TRUE(rows.has_value()) << rows.error().detail;
    ASSERT_EQ(rows->size(), result->steps.size());
    ASSERT_EQ(rows->size(), 2U);

    const auto token = fakeToken();
    for (const auto& row : *rows) {
        EXPECT_FALSE(contains(everything(row), token.substr(20, 40))) << everything(row);
    }
    const auto& pay = (*rows)[1];
    ASSERT_TRUE(pay.request.has_value());
    EXPECT_EQ(pay.request->method, "POST");
    EXPECT_TRUE(contains(pay.request->url, "page=1")) << pay.request->url;
    EXPECT_TRUE(contains(pay.request->body.body, "\"amount\":5")) << pay.request->body.body;
    EXPECT_EQ(pay.httpStatus, 422);
    EXPECT_TRUE(contains(pay.response->body, "declined"));
}

TEST(RunRecording, the_url_user_name_and_credential_query_values_are_masked) {
    auto rig = makeRig();
    auto p = project();
    auto& pay = p.resources.at(ce::ResourceId{"order"}).operations.at("pay");
    pay.pathTemplate = "/pay?api_key=literal-fake-key-0001&page=1";
    ce::RunContext ctx;
    p.environments["local"]["baseUrl"] = "http://user:fake-pass-0001@sut.test";
    const auto result = rig.engine->run(p, ce::OperationId{"order.pay"}, ctx, cliOptions());
    ASSERT_TRUE(result.has_value());
    const auto rows = rig.engine->recordedSteps(result->runId);
    ASSERT_TRUE(rows.has_value());
    const auto& url = rows->back().request->url;
    EXPECT_FALSE(contains(url, "literal-fake-key-0001")) << url;
    EXPECT_FALSE(contains(url, "fake-pass-0001")) << url;
    EXPECT_TRUE(contains(url, "page=1")) << url;
}

TEST(RunRecording, initiator_is_recorded_and_listed) {
    auto rig = makeRig();
    ce::RunContext ctx;
    const auto result =
        rig.engine->run(project(), ce::OperationId{"session.open"}, ctx, cliOptions());
    ASSERT_TRUE(result.has_value());
    const auto runs = rig.engine->listRuns(10);
    ASSERT_TRUE(runs.has_value());
    ASSERT_FALSE(runs->empty());
    EXPECT_EQ(runs->front().runId.value, result->runId.value);
    EXPECT_EQ(runs->front().initiator.kind, ce::RunInitiator::Kind::Agent);
    EXPECT_EQ(runs->front().initiator.name, "test-agent");
    EXPECT_TRUE(runs->front().hasRecordedSteps);
    // The live view reads who started a run from its first event.
    const auto events = rig.engine->historyEvents(result->runId);
    ASSERT_TRUE(events.has_value());
    const auto* started = std::get_if<ce::RunStarted>(&events->front());
    ASSERT_NE(started, nullptr);
    EXPECT_EQ(started->initiator.name, "test-agent");
}

TEST(RunRecording, a_desktop_run_records_no_rows_and_reads_as_the_app) {
    auto rig = makeRig();
    ce::RunContext ctx;
    ce::RunOptions desktop;
    desktop.captureResponseBodies = true;
    const auto result = rig.engine->run(project(), ce::OperationId{"session.open"}, ctx, desktop);
    ASSERT_TRUE(result.has_value());
    const auto rows = rig.engine->recordedSteps(result->runId);
    ASSERT_TRUE(rows.has_value());
    EXPECT_TRUE(rows->empty());
    EXPECT_FALSE(result->steps.front().request.has_value());
    const auto runs = rig.engine->listRuns(1);
    ASSERT_TRUE(runs.has_value());
    EXPECT_FALSE(runs->front().hasRecordedSteps);
    EXPECT_EQ(runs->front().initiator.kind, ce::RunInitiator::Kind::App);
}

TEST(RunRecording, default_history_path_honours_the_test_config_directory) {
    const auto testDir = ce::configDirectoryForTesting();
    ASSERT_TRUE(testDir.has_value()) << "test executables isolate the config directory";
    const auto path = ce::defaultHistoryDatabasePath(fs::temp_directory_path());
    ASSERT_TRUE(path.has_value());
    EXPECT_EQ(path->parent_path(), *testDir / "history");
    EXPECT_TRUE(path->filename().string().starts_with("history-"));
}

namespace {

/// The SQLite store, except that it cannot reserve a run id (a busy database), so the engine
/// falls back to its in-process counter.
class NoAllocatorStore final : public ce::HistoryStore {
public:
    NoAllocatorStore() : inner_(ce::makeSqliteHistoryStore()) {}
    std::expected<void, ce::ReqloomError> open(const fs::path& db) override {
        return inner_->open(db);
    }
    std::expected<ce::RunId, ce::ReqloomError> allocateRunId() override {
        return std::unexpected(
            ce::ReqloomError{ce::ErrorCode::Internal, ce::ErrorClass::Run, "busy"});
    }
    std::expected<void, ce::ReqloomError> append(const ce::RunEvent& e) override {
        return inner_->append(e);
    }
    std::expected<std::vector<ce::RunEvent>, ce::ReqloomError> eventsFor(
        ce::RunId run) const override {
        return inner_->eventsFor(run);
    }
    std::expected<std::vector<ce::RunHistoryRow>, ce::ReqloomError> listRuns(
        std::size_t limit) const override {
        return inner_->listRuns(limit);
    }
    std::expected<void, ce::ReqloomError> appendSteps(
        ce::RunId run, const std::vector<ce::StepResult>& steps) override {
        return inner_->appendSteps(run, steps);
    }
    std::expected<std::vector<ce::StepResult>, ce::ReqloomError> stepsFor(
        ce::RunId run) const override {
        return inner_->stepsFor(run);
    }
    std::expected<void, ce::ReqloomError> clear() override { return inner_->clear(); }
    void close() override { inner_->close(); }

private:
    std::unique_ptr<ce::HistoryStore> inner_;
};

}  // namespace

TEST(RunRecording, a_run_whose_id_was_not_reserved_records_no_rows_over_another_run) {
    auto rig = makeRig();
    // An earlier run (as the app records it) owns the first id in this database.
    ce::RunContext first;
    auto desktop = ce::RunOptions{};
    const auto earlier =
        rig.engine->run(project(), ce::OperationId{"session.open"}, first, desktop);
    ASSERT_TRUE(earlier.has_value());

    auto http = std::make_unique<ScriptedHttp>();
    http->routes["/sessions"] = respond(201, R"({"data":{"sid":"fake"}})");
    ce::ExecutionEngine::Dependencies deps{
        std::move(http), nullptr, std::make_unique<NoAllocatorStore>(), nullptr, nullptr};
    deps.hostApprovals = std::make_unique<reqloom::tests::MemoryHostApprovals>(true);
    ce::ExecutionEngine cli{std::move(deps)};
    ASSERT_TRUE(cli.openHistory(rig.dir / "history.db").has_value());
    ce::RunContext ctx;
    const auto result = cli.run(project(), ce::OperationId{"session.open"}, ctx, cliOptions());
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->recorded);
    // The earlier run keeps no rows of the CLI's: show would print them under its metadata.
    const auto rows = rig.engine->recordedSteps(earlier->runId);
    ASSERT_TRUE(rows.has_value());
    EXPECT_TRUE(rows->empty());
}

TEST(RunRecording, the_store_refuses_rows_for_a_run_it_did_not_start) {
    const auto dir = reqloom::tests::uniqueTempPath("reqloom-recording-store");
    fs::create_directories(dir);
    auto store = ce::makeSqliteHistoryStore();
    ASSERT_TRUE(store->open(dir / "h.db").has_value());
    const auto now = std::chrono::system_clock::now();
    ASSERT_TRUE(store->append(ce::RunStarted{ce::RunId{5}, ce::OperationId{"a.b"}, 1, "e", now, {}})
                    .has_value());
    ce::StepResult row;
    row.op = ce::OperationId{"a.b"};
    EXPECT_TRUE(store->appendSteps(ce::RunId{5}, {row}).has_value());
    // Another connection did not start run 5, and nobody started run 6.
    auto other = ce::makeSqliteHistoryStore();
    ASSERT_TRUE(other->open(dir / "h.db").has_value());
    EXPECT_FALSE(other->appendSteps(ce::RunId{5}, {row}).has_value());
    EXPECT_FALSE(store->appendSteps(ce::RunId{6}, {row}).has_value());
    // A RunStarted refused because the id belongs to another run does not open it either.
    EXPECT_FALSE(
        other->append(ce::RunStarted{ce::RunId{5}, ce::OperationId{"x.y"}, 1, "e", now, {}})
            .has_value());
    EXPECT_FALSE(other->appendSteps(ce::RunId{5}, {row}).has_value());
}

// A token extracted in step 1 becomes a credential only when step 2 sends it. Its events were
// written before that; at the end of a CLI run they are masked again like the recorded rows.
TEST(RunRecording, a_cli_runs_stored_events_hold_no_value_its_rows_mask) {
    // Only a hook makes the id a credential, so nothing marks it as one before step 2 sends it.
    const auto dir = reqloom::tests::uniqueTempPath("reqloom-recording-hook");
    fs::create_directories(dir);
    auto http = std::make_unique<ScriptedHttp>();
    const auto token = fakeToken();
    http->routes["/sessions"] = respond(201, R"({"data":{"sid":")" + token + R"("}})");
    http->routes["/pay"] = respond(422, R"({"error":"declined"})");
    ce::ExecutionEngine::Dependencies deps{std::move(http),
                                           nullptr,
                                           ce::makeSqliteHistoryStore(),
                                           nullptr,
                                           ce::makeQuickJsHookRunner()};
    deps.hostApprovals = std::make_unique<reqloom::tests::MemoryHostApprovals>(true);
    ce::ExecutionEngine engine{std::move(deps)};
    ASSERT_TRUE(engine.openHistory(dir / "history.db").has_value());
    auto p = project();
    auto& pay = p.resources.at(ce::ResourceId{"order"}).operations.at("pay");
    pay.pathTemplate = "/pay?ref={{session.sid}}";
    pay.headers.clear();
    pay.bodyTemplate.reset();
    pay.preRequestScript =
        "ctx.request.headers['Authorization'] = 'Bearer ' + ctx.request.url.split('ref=')[1];";
    ce::RunContext ctx;
    const auto result = engine.run(p, ce::OperationId{"order.pay"}, ctx, cliOptions());
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->recorded);
    // The recorded rows mask it (the final pass knows the credential)...
    for (const auto& row : *engine.recordedSteps(result->runId)) {
        EXPECT_FALSE(contains(everything(row), token.substr(20, 40))) << everything(row);
    }
    ASSERT_TRUE(result.has_value());
    // ...and so do the events stored while the run went.
    const auto events = engine.historyEvents(result->runId);
    ASSERT_TRUE(events.has_value());
    const auto piece = token.substr(20, 40);
    bool sawExtraction = false;
    for (const auto& event : *events) {
        if (const auto* e = std::get_if<ce::ExtractionCompleted>(&event)) {
            sawExtraction = true;
            EXPECT_FALSE(contains(e->value, piece)) << e->value;
        }
        if (const auto* e = std::get_if<ce::StepFailed>(&event)) {
            EXPECT_FALSE(contains(e->detail, piece)) << e->detail;
        }
        if (const auto* e = std::get_if<ce::ResponseReceived>(&event); e != nullptr && e->body) {
            EXPECT_FALSE(contains(*e->body, piece)) << *e->body;
        }
    }
    EXPECT_TRUE(sawExtraction);
}
