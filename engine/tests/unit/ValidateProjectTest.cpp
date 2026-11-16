// Unit tests for the public `validateProject` API. Validating an already-built
// Project must apply the same cycle / undefined-reference checks parseProject
// runs at load — so the desktop editor can reject a bad in-memory edit before
// writing it to disk. No I/O; Projects are built in memory.
//
// Each test fails on the parent commit: `validateProject` is a new symbol.
#include <reqloom/engine/ErrorCodes.h>
#include <reqloom/engine/ExecutionEngine.h>
#include <reqloom/engine/Factories.h>
#include <reqloom/engine/Operation.h>

#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ce = reqloom::engine;

namespace {

ce::Operation makeOp(const std::string& resourceName,
                     const std::string& opName,
                     std::vector<ce::Extraction> extractions = {},
                     std::vector<ce::OperationId> explicitDeps = {}) {
    ce::Operation op;
    op.id = ce::OperationId{resourceName + "." + opName};
    op.resource = ce::ResourceId{resourceName};
    op.actor = ce::ActorId{"user"};
    op.method = ce::HttpMethod::Get;
    op.pathTemplate = "/api/v1/" + resourceName;
    op.extractions = std::move(extractions);
    op.explicitDependencies = std::move(explicitDeps);
    return op;
}

ce::Extraction jsonExt(std::string name, std::string path) {
    return {std::move(name), std::move(path), ce::Extraction::Source::JsonPath};
}

ce::Project makeProject(std::map<std::string, std::map<std::string, ce::Operation>> resources) {
    ce::Project p;
    p.name = "TestProject";
    p.defaultEnvironment = "local";
    p.environments["local"] = {{"baseUrl", "http://localhost:0"}};

    ce::Actor user;
    user.id = ce::ActorId{"user"};
    user.strategy = ce::AuthStrategy::Simple;
    ce::AuthStep step;
    step.id = "login";
    step.method = ce::HttpMethod::Post;
    step.pathTemplate = "/api/v1/auth/login";
    step.expectStatus = 200;
    step.extractions.push_back(jsonExt("token", "$.token"));
    user.authSteps.push_back(std::move(step));
    p.actors[user.id] = std::move(user);

    for (auto& [resName, ops] : resources) {
        ce::Resource res;
        res.id = ce::ResourceId{resName};
        for (auto& [opName, op] : ops) {
            res.operations[opName] = std::move(op);
        }
        p.resources[res.id] = std::move(res);
    }
    return p;
}

}  // namespace

TEST(ValidateProject, valid_project_passes) {
    auto project = makeProject({
        {"product", {{"get", makeOp("product", "get")}}},
    });

    EXPECT_TRUE(ce::validateProject(project).has_value());
}

TEST(ValidateProject, explicit_dependency_cycle_is_rejected) {
    // a depends_on b, b depends_on a — a back edge through explicit deps.
    auto project = makeProject({
        {"order",
         {
             {"a", makeOp("order", "a", {}, {ce::OperationId{"order.b"}})},
             {"b", makeOp("order", "b", {}, {ce::OperationId{"order.a"}})},
         }},
    });

    auto result = ce::validateProject(project);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::Cycle);
}

TEST(ValidateProject, implicit_template_cycle_is_rejected) {
    // No depends_on at all: the cycle is formed purely through {{ref}} edges.
    // order.a extracts order_id and consumes {{order.b_id}}; order.b extracts
    // b_id and consumes {{order.order_id}} — a → b → a via templates.
    auto opA = makeOp("order", "a", {jsonExt("order_id", "$.id")});
    opA.pathTemplate = "/api/v1/order/{{order.b_id}}";
    auto opB = makeOp("order", "b", {jsonExt("b_id", "$.id")});
    opB.pathTemplate = "/api/v1/order/{{order.order_id}}";

    auto project = makeProject({
        {"order", {{"a", std::move(opA)}, {"b", std::move(opB)}}},
    });

    auto result = ce::validateProject(project);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::Cycle);
}

TEST(ValidateProject, undefined_reference_is_rejected) {
    // Consumes {{product.product_id}} but nothing extracts it.
    auto op = makeOp("order", "create");
    op.pathTemplate = "/api/v1/order/{{product.product_id}}";

    auto project = makeProject({
        {"order", {{"create", std::move(op)}}},
    });

    auto result = ce::validateProject(project);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::RefUndefined);
}

// ─── D-04: unknown actor / poll actor / for_each resource ───────────────────

TEST(ValidateProject, unknown_operation_actor_is_rejected) {
    auto op = makeOp("order", "create");
    op.actor = ce::ActorId{"ghost"};
    auto project = makeProject({{"order", {{"create", std::move(op)}}}});

    auto result = ce::validateProject(project);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::RefUndefined);
    EXPECT_NE(result.error().detail.find("order.create"), std::string::npos)
        << result.error().detail;
    EXPECT_NE(result.error().detail.find("ghost"), std::string::npos) << result.error().detail;
}

TEST(ValidateProject, operation_without_actor_is_still_valid) {
    auto op = makeOp("order", "create");
    op.actor = ce::ActorId{};
    auto project = makeProject({{"order", {{"create", std::move(op)}}}});

    EXPECT_TRUE(ce::validateProject(project).has_value());
}

TEST(ValidateProject, unknown_poll_until_actor_is_rejected) {
    auto op = makeOp("job", "start");
    ce::PollUntil poll;
    poll.pathTemplate = "/api/v1/jobs/1";
    poll.successWhen = "$.status == 'done'";
    poll.actor = ce::ActorId{"phantom"};
    op.pollUntil = std::move(poll);
    auto project = makeProject({{"job", {{"start", std::move(op)}}}});

    auto result = ce::validateProject(project);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::RefUndefined);
    EXPECT_NE(result.error().detail.find("job.start"), std::string::npos) << result.error().detail;
    EXPECT_NE(result.error().detail.find("phantom"), std::string::npos) << result.error().detail;
}

TEST(ValidateProject, poll_until_with_known_actor_and_response_ref_is_valid) {
    auto op = makeOp("job", "start");
    ce::PollUntil poll;
    poll.pathTemplate = "{{response.headers.Location}}";
    poll.successWhen = "$.status == 'done'";
    poll.actor = ce::ActorId{"user"};
    op.pollUntil = std::move(poll);
    auto project = makeProject({{"job", {{"start", std::move(op)}}}});

    auto result = ce::validateProject(project);
    EXPECT_TRUE(result.has_value()) << result.error().detail;
}

TEST(ValidateProject, unknown_for_each_resource_is_rejected) {
    auto op = makeOp("item", "check");
    op.forEach = ce::ForEach{ce::ResourceId{"nowhere"}, false};
    auto project = makeProject({{"item", {{"check", std::move(op)}}}});

    auto result = ce::validateProject(project);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::RefUndefined);
    EXPECT_NE(result.error().detail.find("item.check"), std::string::npos) << result.error().detail;
    EXPECT_NE(result.error().detail.find("nowhere"), std::string::npos) << result.error().detail;
}

// ─── Load-time predicate / regex / faker checks (items 3, 7, 10) ───────────

namespace {

ce::Project projectWith(ce::Operation op) {
    return makeProject({{"item", {{"check", std::move(op)}}}});
}

}  // namespace

TEST(ValidateProject, malformed_assertion_is_a_load_error_naming_op_field_and_reason) {
    auto op = makeOp("item", "check");
    op.assertions.push_back({"$.status_code == 200", std::nullopt});
    op.assertions.push_back({"$.total >", std::string{"total positive"}});
    auto result = ce::validateProject(projectWith(std::move(op)));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::SchemaInvalid);
    const auto& d = result.error().detail;
    EXPECT_NE(d.find("item.check"), std::string::npos) << d;
    EXPECT_NE(d.find("assert[1]"), std::string::npos) << d;
    EXPECT_NE(d.find("total positive"), std::string::npos) << d;
    EXPECT_NE(d.find("expected term"), std::string::npos) << d;
}

TEST(ValidateProject, malformed_poll_conditions_are_load_errors) {
    auto op = makeOp("item", "check");
    op.pollUntil = ce::PollUntil{};
    op.pollUntil->successWhen = "$.state == 'done' &&";
    auto bad = ce::validateProject(projectWith(op));
    ASSERT_FALSE(bad.has_value());
    EXPECT_NE(bad.error().detail.find("poll_until.success_when"), std::string::npos)
        << bad.error().detail;

    op.pollUntil->successWhen = "$.state == 'done'";
    op.pollUntil->failWhen = "$.state = 'failed'";
    auto badFail = ce::validateProject(projectWith(op));
    ASSERT_FALSE(badFail.has_value());
    EXPECT_NE(badFail.error().detail.find("poll_until.fail_when"), std::string::npos)
        << badFail.error().detail;

    op.pollUntil->failWhen = "($.state == 'failed' || $.state == 'cancelled')";
    auto good = ce::validateProject(projectWith(op));
    EXPECT_TRUE(good.has_value()) << good.error().detail;
}

TEST(ValidateProject, predicates_with_templates_are_not_falsely_rejected) {
    auto op = makeOp("item", "check");
    op.assertions.push_back({"$.owner == '{{secret.API_KEY}}'", std::nullopt});
    op.assertions.push_back({"$.count == {{env.baseUrl}}", std::nullopt});
    auto result = ce::validateProject(projectWith(std::move(op)));
    EXPECT_TRUE(result.has_value()) << result.error().detail;
}

TEST(ValidateProject, guarded_regex_is_rejected_at_load_in_matches_and_extraction) {
    auto matchesOp = makeOp("item", "check");
    matchesOp.assertions.push_back({"$.id matches '(a+)+$'", std::nullopt});
    auto m = ce::validateProject(projectWith(std::move(matchesOp)));
    ASSERT_FALSE(m.has_value());
    EXPECT_NE(m.error().detail.find("(a+)+$"), std::string::npos) << m.error().detail;

    auto extractOp = makeOp("item", "check");
    extractOp.extractions.push_back({"tok", "(\\w+\\s*)*end", ce::Extraction::Source::Regex});
    auto e = ce::validateProject(projectWith(std::move(extractOp)));
    ASSERT_FALSE(e.has_value());
    EXPECT_NE(e.error().detail.find("extract 'tok'"), std::string::npos) << e.error().detail;
    EXPECT_NE(e.error().detail.find("item.check"), std::string::npos) << e.error().detail;

    auto safeOp = makeOp("item", "check");
    safeOp.extractions.push_back(
        {"tok", R"re("token":"([^"]+)")re", ce::Extraction::Source::Regex});
    auto ok = ce::validateProject(projectWith(std::move(safeOp)));
    EXPECT_TRUE(ok.has_value()) << ok.error().detail;
}

TEST(ValidateProject, unknown_faker_name_is_a_load_error_listing_valid_names) {
    auto op = makeOp("item", "check");
    op.bodyTemplate = R"({"email":"{{$.faker.emial}}"})";
    auto result = ce::validateProject(projectWith(op));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::RefUndefined);
    EXPECT_NE(result.error().detail.find("emial"), std::string::npos) << result.error().detail;
    EXPECT_NE(result.error().detail.find("email, phone"), std::string::npos)
        << result.error().detail;

    op.bodyTemplate = R"({"email":"{{$.faker.email}}","p":"{{ $.faker.phone }}"})";
    auto ok = ce::validateProject(projectWith(op));
    EXPECT_TRUE(ok.has_value()) << ok.error().detail;
}
