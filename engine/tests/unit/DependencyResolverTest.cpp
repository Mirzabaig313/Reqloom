// Unit tests for DependencyResolver::resolve().
//
// Builds minimal in-memory Project values and asserts the resolved chain
// order, cycle detection, and undefined-reference errors. No I/O.
//
// Each test fails on the parent commit if the resolver is broken.
#include "domain/DependencyResolver.h"

#include <chainapi/engine/ErrorCodes.h>
#include <chainapi/engine/ExecutionEngine.h>
#include <chainapi/engine/Operation.h>

#include <gtest/gtest.h>

namespace ce = chainapi::engine;

namespace {

// ─── Helpers ─────────────────────────────────────────────────────────────────

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

/// Build a Project with a single actor "user" and the given resources.
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

// ─── Single operation (no deps) ──────────────────────────────────────────────

TEST(DependencyResolver, single_op_with_no_deps_resolves_to_itself) {
    auto project = makeProject({
        {"product", {{"get", makeOp("product", "get")}}},
    });

    ce::DependencyResolver resolver;
    auto chain = resolver.resolve(project, ce::OperationId{"product.get"});

    ASSERT_TRUE(chain.has_value()) << chain.error().detail;
    ASSERT_EQ(chain->size(), 1u);
    EXPECT_EQ((*chain)[0].value, "product.get");
}

// ─── Implicit dependency via template reference ───────────────────────────────

TEST(DependencyResolver, implicit_dep_from_path_template_is_resolved) {
    // order.pay references {{product.product_id}} in its path template.
    // product.create extracts product_id. So order.pay depends on product.create.
    auto productCreate = makeOp("product", "create", {jsonExt("product_id", "$.id")});
    auto orderPay = makeOp("order", "pay");
    orderPay.pathTemplate = "/api/v1/orders/{{product.product_id}}/pay";

    auto project = makeProject({
        {"product", {{"create", std::move(productCreate)}}},
        {"order", {{"pay", std::move(orderPay)}}},
    });

    ce::DependencyResolver resolver;
    auto chain = resolver.resolve(project, ce::OperationId{"order.pay"});

    ASSERT_TRUE(chain.has_value()) << chain.error().detail;
    ASSERT_EQ(chain->size(), 2u);

    // product.create must come before order.pay.
    EXPECT_EQ(chain->front().value, "product.create");
    EXPECT_EQ(chain->back().value, "order.pay");
}

// ─── Explicit dependency ──────────────────────────────────────────────────────

TEST(DependencyResolver, explicit_depends_on_is_included_in_chain) {
    auto productPublish = makeOp("product", "publish");
    auto productCreate = makeOp("product", "create", {jsonExt("product_id", "$.id")});

    // publish explicitly depends on create even though it doesn't reference
    // any extracted variable in its templates.
    productPublish.explicitDependencies = {ce::OperationId{"product.create"}};

    auto project = makeProject({
        {"product", {{"create", std::move(productCreate)}, {"publish", std::move(productPublish)}}},
    });

    ce::DependencyResolver resolver;
    auto chain = resolver.resolve(project, ce::OperationId{"product.publish"});

    ASSERT_TRUE(chain.has_value()) << chain.error().detail;
    ASSERT_EQ(chain->size(), 2u);
    EXPECT_EQ(chain->front().value, "product.create");
    EXPECT_EQ(chain->back().value, "product.publish");
}

// ─── Multi-hop chain ─────────────────────────────────────────────────────────

TEST(DependencyResolver, three_hop_chain_is_ordered_correctly) {
    // refund.approve → refund.request → order.pay → order.create
    auto orderCreate = makeOp("order", "create", {jsonExt("order_id", "$.id")});
    auto orderPay = makeOp("order", "pay", {jsonExt("payment_id", "$.id")});
    orderPay.pathTemplate = "/api/v1/orders/{{order.order_id}}/pay";

    auto refundRequest = makeOp("refund", "request", {jsonExt("refund_id", "$.id")});
    refundRequest.pathTemplate = "/api/v1/refunds/{{order.payment_id}}";

    auto refundApprove = makeOp("refund", "approve");
    refundApprove.pathTemplate = "/api/v1/refunds/{{refund.refund_id}}/approve";

    auto project = makeProject({
        {"order", {{"create", std::move(orderCreate)}, {"pay", std::move(orderPay)}}},
        {"refund", {{"request", std::move(refundRequest)}, {"approve", std::move(refundApprove)}}},
    });

    ce::DependencyResolver resolver;
    auto chain = resolver.resolve(project, ce::OperationId{"refund.approve"});

    ASSERT_TRUE(chain.has_value()) << chain.error().detail;
    ASSERT_EQ(chain->size(), 4u);

    auto pos = [&](const std::string& v) -> std::size_t {
        for (std::size_t i = 0; i < chain->size(); ++i) {
            if ((*chain)[i].value == v) return i;
        }
        return std::string::npos;
    };
    EXPECT_LT(pos("order.create"), pos("order.pay"));
    EXPECT_LT(pos("order.pay"), pos("refund.request"));
    EXPECT_LT(pos("refund.request"), pos("refund.approve"));
    EXPECT_EQ(chain->back().value, "refund.approve");
}

// ─── Undefined reference ─────────────────────────────────────────────────────

TEST(DependencyResolver, undefined_resource_returns_ref_undefined_error) {
    auto project = makeProject({
        {"order", {{"pay", makeOp("order", "pay")}}},
    });

    ce::DependencyResolver resolver;
    auto chain = resolver.resolve(project, ce::OperationId{"nonexistent.op"});

    ASSERT_FALSE(chain.has_value());
    EXPECT_EQ(chain.error().code, ce::ErrorCode::RefUndefined);
    EXPECT_EQ(chain.error().cls, ce::ErrorClass::Schema);
    EXPECT_NE(chain.error().detail.find("nonexistent"), std::string::npos);
}

TEST(DependencyResolver, undefined_operation_within_known_resource_returns_ref_undefined) {
    auto project = makeProject({
        {"order", {{"create", makeOp("order", "create")}}},
    });

    ce::DependencyResolver resolver;
    auto chain = resolver.resolve(project, ce::OperationId{"order.nonexistent"});

    ASSERT_FALSE(chain.has_value());
    EXPECT_EQ(chain.error().code, ce::ErrorCode::RefUndefined);
    EXPECT_NE(chain.error().detail.find("order.nonexistent"), std::string::npos);
}

TEST(DependencyResolver, malformed_operation_id_without_dot_returns_ref_undefined) {
    auto project = makeProject({
        {"order", {{"create", makeOp("order", "create")}}},
    });

    ce::DependencyResolver resolver;
    auto chain = resolver.resolve(project, ce::OperationId{"nodot"});

    ASSERT_FALSE(chain.has_value());
    EXPECT_EQ(chain.error().code, ce::ErrorCode::RefUndefined);
}

// ─── Cycle detection ─────────────────────────────────────────────────────────

TEST(DependencyResolver, direct_cycle_via_explicit_deps_returns_cycle_error) {
    // a.op explicitly depends on b.op, and b.op explicitly depends on a.op.
    auto aOp = makeOp("a", "op", {}, {ce::OperationId{"b.op"}});
    auto bOp = makeOp("b", "op", {}, {ce::OperationId{"a.op"}});

    auto project = makeProject({
        {"a", {{"op", std::move(aOp)}}},
        {"b", {{"op", std::move(bOp)}}},
    });

    ce::DependencyResolver resolver;
    auto chain = resolver.resolve(project, ce::OperationId{"a.op"});

    ASSERT_FALSE(chain.has_value());
    EXPECT_EQ(chain.error().code, ce::ErrorCode::Cycle);
    EXPECT_EQ(chain.error().cls, ce::ErrorClass::Schema);
}

TEST(DependencyResolver, implicit_cycle_via_template_references_returns_cycle_error) {
    // a.op extracts "a_id" and references {{b.b_id}} in its path.
    // b.op extracts "b_id" and references {{a.a_id}} in its path.
    auto aOp = makeOp("a", "op", {jsonExt("a_id", "$.id")});
    aOp.pathTemplate = "/api/v1/a/{{b.b_id}}";

    auto bOp = makeOp("b", "op", {jsonExt("b_id", "$.id")});
    bOp.pathTemplate = "/api/v1/b/{{a.a_id}}";

    auto project = makeProject({
        {"a", {{"op", std::move(aOp)}}},
        {"b", {{"op", std::move(bOp)}}},
    });

    ce::DependencyResolver resolver;
    auto chain = resolver.resolve(project, ce::OperationId{"a.op"});

    ASSERT_FALSE(chain.has_value());
    EXPECT_EQ(chain.error().code, ce::ErrorCode::Cycle);
}

// ─── Actor references are not treated as resource deps ───────────────────────

TEST(DependencyResolver, actor_variable_reference_does_not_create_resource_dep) {
    // {{user.token}} in a path template must NOT be treated as a dependency
    // on a resource named "user" — actors are handled by the session system.
    auto op = makeOp("product", "get");
    op.pathTemplate = "/api/v1/products";
    op.headers["Authorization"] = "Bearer {{user.token}}";

    auto project = makeProject({
        {"product", {{"get", std::move(op)}}},
    });

    ce::DependencyResolver resolver;
    auto chain = resolver.resolve(project, ce::OperationId{"product.get"});

    ASSERT_TRUE(chain.has_value()) << chain.error().detail;
    ASSERT_EQ(chain->size(), 1u);
    EXPECT_EQ((*chain)[0].value, "product.get");
}

// ─── Env / secret references are not treated as resource deps ────────────────

TEST(DependencyResolver, env_and_secret_references_do_not_create_deps) {
    auto op = makeOp("product", "get");
    op.pathTemplate = "{{env.baseUrl}}/api/v1/products";
    op.headers["X-Key"] = "{{secret.API_KEY}}";

    auto project = makeProject({
        {"product", {{"get", std::move(op)}}},
    });

    ce::DependencyResolver resolver;
    auto chain = resolver.resolve(project, ce::OperationId{"product.get"});

    ASSERT_TRUE(chain.has_value()) << chain.error().detail;
    ASSERT_EQ(chain->size(), 1u);
}

// ─── Body template references ─────────────────────────────────────────────────

TEST(DependencyResolver, implicit_dep_from_body_template_is_resolved) {
    auto orderCreate = makeOp("order", "create", {jsonExt("order_id", "$.id")});
    auto paymentCreate = makeOp("payment", "create");
    paymentCreate.bodyTemplate = R"({"order_id": "{{order.order_id}}"})";

    auto project = makeProject({
        {"order", {{"create", std::move(orderCreate)}}},
        {"payment", {{"create", std::move(paymentCreate)}}},
    });

    ce::DependencyResolver resolver;
    auto chain = resolver.resolve(project, ce::OperationId{"payment.create"});

    ASSERT_TRUE(chain.has_value()) << chain.error().detail;
    ASSERT_EQ(chain->size(), 2u);
    EXPECT_EQ(chain->front().value, "order.create");
    EXPECT_EQ(chain->back().value, "payment.create");
}

// ─── Indexed reference {{R[N].x}} does not break resolution ──────────────────

TEST(DependencyResolver, indexed_resource_reference_creates_correct_dep) {
    auto orderCreate = makeOp("order", "create", {jsonExt("order_id", "$.id")});
    auto refundCreate = makeOp("refund", "create");
    // Uses indexed form {{order[1].order_id}} — still a dep on order.create.
    refundCreate.pathTemplate = "/api/v1/refunds/{{order[1].order_id}}";

    auto project = makeProject({
        {"order", {{"create", std::move(orderCreate)}}},
        {"refund", {{"create", std::move(refundCreate)}}},
    });

    ce::DependencyResolver resolver;
    auto chain = resolver.resolve(project, ce::OperationId{"refund.create"});

    ASSERT_TRUE(chain.has_value()) << chain.error().detail;
    ASSERT_EQ(chain->size(), 2u);
    EXPECT_EQ(chain->front().value, "order.create");
    EXPECT_EQ(chain->back().value, "refund.create");
}
