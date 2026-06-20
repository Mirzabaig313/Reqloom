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
