// Unit tests for DependencyResolver::resolve().
//
// Builds minimal in-memory Project values and asserts the resolved chain
// order, cycle detection, and undefined-reference errors. No I/O.
//
// Each test fails on the parent commit if the resolver is broken.
#include "domain/DependencyResolver.h"

#include <reqloom/engine/ErrorCodes.h>
#include <reqloom/engine/ExecutionEngine.h>
#include <reqloom/engine/Operation.h>

#include <gtest/gtest.h>

#include <set>
#include <string>

namespace ce = reqloom::engine;

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

// ─── resolvePlan: edges (explicit vs implicit) ───────────────────────────────

TEST(DependencyResolver, resolve_plan_tags_implicit_edge_with_flowing_variable) {
    // order.pay references {{product.product_id}}; product.create extracts it.
    auto productCreate = makeOp("product", "create", {jsonExt("product_id", "$.id")});
    auto orderPay = makeOp("order", "pay");
    orderPay.pathTemplate = "/api/v1/orders/{{product.product_id}}/pay";

    auto project = makeProject({
        {"product", {{"create", std::move(productCreate)}}},
        {"order", {{"pay", std::move(orderPay)}}},
    });

    ce::DependencyResolver resolver;
    auto plan = resolver.resolvePlan(project, ce::OperationId{"order.pay"});

    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    EXPECT_EQ(plan->order.front().value, "product.create");
    EXPECT_EQ(plan->order.back().value, "order.pay");

    ASSERT_EQ(plan->edges.size(), 1u);
    const auto& edge = plan->edges.front();
    EXPECT_EQ(edge.consumer.value, "order.pay");
    EXPECT_EQ(edge.producer.value, "product.create");
    EXPECT_TRUE(edge.implicit);
    EXPECT_EQ(edge.variable, "product.product_id");
}

TEST(DependencyResolver, resolve_plan_marks_explicit_edge_without_variable) {
    auto productPublish = makeOp("product", "publish");
    productPublish.explicitDependencies = {ce::OperationId{"product.create"}};
    auto productCreate = makeOp("product", "create", {jsonExt("product_id", "$.id")});

    auto project = makeProject({
        {"product", {{"publish", std::move(productPublish)}, {"create", std::move(productCreate)}}},
    });

    ce::DependencyResolver resolver;
    auto plan = resolver.resolvePlan(project, ce::OperationId{"product.publish"});

    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    ASSERT_EQ(plan->edges.size(), 1u);
    const auto& edge = plan->edges.front();
    EXPECT_EQ(edge.consumer.value, "product.publish");
    EXPECT_EQ(edge.producer.value, "product.create");
    EXPECT_FALSE(edge.implicit);
    EXPECT_TRUE(edge.variable.empty());
}

// ─── Explicit dependency ──────────────────────────────────────────────────────

TEST(DependencyResolver, suggest_variables_includes_extracts_actor_env_and_builtins) {
    // order.pay references {{product.product_id}}; product.create extracts it.
    auto productCreate = makeOp("product", "create", {jsonExt("product_id", "$.id")});
    auto orderPay = makeOp("order", "pay");
    orderPay.pathTemplate = "/api/v1/orders/{{product.product_id}}/pay";

    auto project = makeProject({
        {"product", {{"create", std::move(productCreate)}}},
        {"order", {{"pay", std::move(orderPay)}}},
    });

    ce::DependencyResolver resolver;
    const ce::OperationId target{"order.pay"};
    auto plan = resolver.resolvePlan(project, target);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;

    const auto suggestions = ce::collectVariableSuggestions(project, target, *plan, "");

    std::set<std::string> tokens;
    for (const auto& s : suggestions) {
        tokens.insert(s.token);
    }
    EXPECT_TRUE(tokens.contains("product.product_id"));  // upstream extract
    EXPECT_TRUE(tokens.contains("user.token"));          // actor session token
    EXPECT_TRUE(tokens.contains("env.baseUrl"));         // environment var
    EXPECT_TRUE(tokens.contains("$.uuid"));              // built-in
    EXPECT_TRUE(tokens.contains("$.now"));

    // The producing op is named as the detail of its extract suggestion.
    for (const auto& s : suggestions) {
        if (s.token == "product.product_id") {
            EXPECT_EQ(s.kind, ce::VariableSuggestion::Kind::Extract);
            EXPECT_EQ(s.detail, "product.create");
        }
    }
}

TEST(DependencyResolver, suggest_variables_excludes_the_targets_own_extractions) {
    // A target can't reference a value it hasn't produced yet, so its own
    // extractions are not suggested (only upstream producers' are).
    auto orderCreate =
        makeOp("order", "create", {jsonExt("order_id", "$.id"), jsonExt("total", "$.total")});

    auto project = makeProject({
        {"order", {{"create", std::move(orderCreate)}}},
    });

    ce::DependencyResolver resolver;
    const ce::OperationId target{"order.create"};
    auto plan = resolver.resolvePlan(project, target);
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;

    const auto suggestions = ce::collectVariableSuggestions(project, target, *plan, "");
    std::set<std::string> tokens;
    for (const auto& s : suggestions) {
        tokens.insert(s.token);
    }
    EXPECT_FALSE(tokens.contains("order.order_id"));
    EXPECT_FALSE(tokens.contains("order.total"));
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

// ─── collectSecretReferences ─────────────────────────────────────────────────

TEST(DependencyResolver, collect_secret_references_finds_op_template_secrets) {
    auto op = makeOp("product", "get");
    op.headers["X-Api-Key"] = "{{secret.API_KEY}}";
    op.bodyTemplate = R"({"sig": "{{secret.SIGNING_KEY}}"})";

    auto project = makeProject({
        {"product", {{"get", std::move(op)}}},
    });

    const auto names = ce::DependencyResolver::collectSecretReferences(project);
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], "API_KEY");  // sorted
    EXPECT_EQ(names[1], "SIGNING_KEY");
}

TEST(DependencyResolver, collect_secret_references_dedups_across_fields) {
    auto op = makeOp("product", "get");
    op.headers["X-Api-Key"] = "{{secret.API_KEY}}";
    op.queryParams["k"] = "{{secret.API_KEY}}";

    auto project = makeProject({
        {"product", {{"get", std::move(op)}}},
    });

    const auto names = ce::DependencyResolver::collectSecretReferences(project);
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], "API_KEY");
}

TEST(DependencyResolver, collect_secret_references_scans_actor_auth_and_inject) {
    auto project = makeProject({
        {"product", {{"get", makeOp("product", "get")}}},
    });
    // Augment the default "user" actor with secret-bearing auth + inject.
    auto& user = project.actors[ce::ActorId{"user"}];
    user.authConfig["password"] = "{{secret.USER_PASSWORD}}";
    user.inject.headers["Authorization"] = "Bearer {{secret.STATIC_TOKEN}}";

    const auto names = ce::DependencyResolver::collectSecretReferences(project);
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], "STATIC_TOKEN");
    EXPECT_EQ(names[1], "USER_PASSWORD");
}

TEST(DependencyResolver, collect_secret_references_scans_poll_predicates) {
    auto op = makeOp("job", "status");
    ce::PollUntil poll;
    poll.pathTemplate = "/api/v1/jobs/{{secret.JOB_SIGNER}}";
    poll.successWhen = "$.status == 'done'";
    op.pollUntil = std::move(poll);

    auto project = makeProject({
        {"job", {{"status", std::move(op)}}},
    });

    const auto names = ce::DependencyResolver::collectSecretReferences(project);
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], "JOB_SIGNER");
}

TEST(DependencyResolver, collect_secret_references_empty_when_no_secrets) {
    auto project = makeProject({
        {"product", {{"get", makeOp("product", "get")}}},
    });

    const auto names = ce::DependencyResolver::collectSecretReferences(project);
    EXPECT_TRUE(names.empty());
}

// ─── D-07: several operations extract the same variable ──────────────────────

namespace {

/// An op on `order` whose path consumes {{order.id}} and which also
/// extracts `id` (e.g. GET/PUT /orders/{id} in an imported spec).
ce::Operation consumingExtractor(const std::string& opName) {
    auto op = makeOp("order", opName, {jsonExt("id", "$.id")});
    op.pathTemplate = "/api/v1/orders/{{order.id}}";
    return op;
}

}  // namespace

TEST(DependencyResolver, extractor_that_consumes_the_variable_is_not_its_producer) {
    // update and patch both consume {{order.id}} and re-extract id; only
    // create produces it from scratch. Previously update ↔ patch formed a
    // false cycle.
    auto project = makeProject({
        {"order",
         {
             {"create", makeOp("order", "create", {jsonExt("id", "$.id")})},
             {"update", consumingExtractor("update")},
             {"patch", consumingExtractor("patch")},
         }},
    });

    ce::DependencyResolver resolver;
    auto valid = resolver.validate(project);
    ASSERT_TRUE(valid.has_value()) << valid.error().detail;

    auto chain = resolver.resolve(project, ce::OperationId{"order.update"});
    ASSERT_TRUE(chain.has_value()) << chain.error().detail;
    ASSERT_EQ(chain->size(), 2u);
    EXPECT_EQ((*chain)[0].value, "order.create");
    EXPECT_EQ((*chain)[1].value, "order.update");
}

TEST(DependencyResolver, explicit_depends_on_disambiguates_between_producers) {
    auto get = makeOp("order", "get", {}, {ce::OperationId{"order.import"}});
    get.pathTemplate = "/api/v1/orders/{{order.id}}";
    auto project = makeProject({
        {"order",
         {
             {"create", makeOp("order", "create", {jsonExt("id", "$.id")})},
             {"import", makeOp("order", "import", {jsonExt("id", "$.id")})},
             {"get", std::move(get)},
         }},
    });

    ce::DependencyResolver resolver;
    auto valid = resolver.validate(project);
    ASSERT_TRUE(valid.has_value()) << valid.error().detail;

    auto chain = resolver.resolve(project, ce::OperationId{"order.get"});
    ASSERT_TRUE(chain.has_value()) << chain.error().detail;
    ASSERT_EQ(chain->size(), 2u);
    EXPECT_EQ((*chain)[0].value, "order.import");
    EXPECT_EQ((*chain)[1].value, "order.get");
}

TEST(DependencyResolver, ambiguous_producer_is_reported_not_a_cycle) {
    auto get = makeOp("order", "get");
    get.pathTemplate = "/api/v1/orders/{{order.id}}";
    auto project = makeProject({
        {"order",
         {
             {"create", makeOp("order", "create", {jsonExt("id", "$.id")})},
             {"import", makeOp("order", "import", {jsonExt("id", "$.id")})},
             {"get", std::move(get)},
         }},
    });

    ce::DependencyResolver resolver;
    auto valid = resolver.validate(project);
    ASSERT_FALSE(valid.has_value());
    EXPECT_EQ(valid.error().code, ce::ErrorCode::RefUndefined);
    const auto& detail = valid.error().detail;
    EXPECT_NE(detail.find("ambiguous producer for {{order.id}}"), std::string::npos) << detail;
    EXPECT_NE(detail.find("order.create"), std::string::npos) << detail;
    EXPECT_NE(detail.find("order.import"), std::string::npos) << detail;
    EXPECT_NE(detail.find("depends_on"), std::string::npos) << detail;

    auto chain = resolver.resolve(project, ce::OperationId{"order.get"});
    ASSERT_FALSE(chain.has_value());
    EXPECT_NE(chain.error().detail.find("ambiguous producer"), std::string::npos)
        << chain.error().detail;
}

TEST(DependencyResolver, no_producer_when_every_extractor_also_consumes) {
    auto project = makeProject({
        {"order",
         {
             {"update", consumingExtractor("update")},
             {"patch", consumingExtractor("patch")},
         }},
    });

    ce::DependencyResolver resolver;
    auto valid = resolver.validate(project);
    ASSERT_FALSE(valid.has_value());
    EXPECT_NE(valid.error().code, ce::ErrorCode::Cycle) << valid.error().detail;
    EXPECT_NE(valid.error().detail.find("no producer"), std::string::npos) << valid.error().detail;
    EXPECT_NE(valid.error().detail.find("{{order.id}}"), std::string::npos) << valid.error().detail;
}

TEST(DependencyResolver, sole_extractor_consuming_its_own_variable_is_unchanged) {
    // Only the op itself extracts the variable it consumes: no edge, no
    // error (as before — an op never depends on itself).
    auto project = makeProject({{"order", {{"get", consumingExtractor("get")}}}});

    ce::DependencyResolver resolver;
    auto valid = resolver.validate(project);
    ASSERT_TRUE(valid.has_value()) << valid.error().detail;
    auto chain = resolver.resolve(project, ce::OperationId{"order.get"});
    ASSERT_TRUE(chain.has_value()) << chain.error().detail;
    EXPECT_EQ(chain->size(), 1u);
}

// ─── D-08: builtin call arguments, inline / default auth, poll_until ─────────

TEST(DependencyResolver, collect_secret_references_scans_builtin_call_arguments) {
    auto op = makeOp("product", "get");
    op.headers["Authorization"] = "Basic {{$.base64.encode(secret.API_KEY)}}";
    op.headers["X-Literal"] = "{{$.base64.encode('secret.NOT_A_REF')}}";

    auto project = makeProject({{"product", {{"get", std::move(op)}}}});

    const auto names = ce::DependencyResolver::collectSecretReferences(project);
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], "API_KEY");
}

TEST(DependencyResolver, builtin_call_argument_creates_dependency_edge) {
    auto pay = makeOp("payment", "create");
    pay.headers["X-Sig"] = "{{ $.hex.encode(order.token) }}";
    auto project = makeProject({
        {"order", {{"create", makeOp("order", "create", {jsonExt("token", "$.token")})}}},
        {"payment", {{"create", std::move(pay)}}},
    });

    ce::DependencyResolver resolver;
    auto chain = resolver.resolve(project, ce::OperationId{"payment.create"});
    ASSERT_TRUE(chain.has_value()) << chain.error().detail;
    ASSERT_EQ(chain->size(), 2u);
    EXPECT_EQ((*chain)[0].value, "order.create");
}

TEST(DependencyResolver, validate_rejects_unknown_scope_inside_builtin_argument) {
    auto op = makeOp("product", "get");
    op.headers["X"] = "{{$.url.encode(nope.value)}}";
    auto project = makeProject({{"product", {{"get", std::move(op)}}}});

    ce::DependencyResolver resolver;
    auto valid = resolver.validate(project);
    ASSERT_FALSE(valid.has_value());
    EXPECT_EQ(valid.error().code, ce::ErrorCode::RefUndefined);
}

TEST(DependencyResolver, validate_rejects_env_builtin_not_listed_in_allowed_env) {
    auto op = makeOp("product", "get");
    op.headers["X"] = "{{$.env.AWS_SECRET_ACCESS_KEY}}";
    auto project = makeProject({{"product", {{"get", std::move(op)}}}});
    project.allowedEnv = {"CI_TOKEN"};

    ce::DependencyResolver resolver;
    auto valid = resolver.validate(project);
    ASSERT_FALSE(valid.has_value());
    EXPECT_EQ(valid.error().code, ce::ErrorCode::RefUndefined);
    EXPECT_NE(
        valid.error().detail.find(
            "environment variable AWS_SECRET_ACCESS_KEY is not in allowed_env in reqloom.yaml"),
        std::string::npos)
        << valid.error().detail;
}

TEST(DependencyResolver, validate_accepts_env_builtin_listed_in_allowed_env) {
    auto op = makeOp("product", "get");
    op.headers["X"] = "{{ $.env.CI_TOKEN }}";
    auto project = makeProject({{"product", {{"get", std::move(op)}}}});
    project.allowedEnv = {"CI_TOKEN"};

    ce::DependencyResolver resolver;
    auto valid = resolver.validate(project);
    EXPECT_TRUE(valid.has_value()) << valid.error().detail;
}

TEST(DependencyResolver, collect_secret_references_scans_inline_and_default_auth) {
    auto op = makeOp("product", "get");
    ce::InlineAuth inlineAuth;
    inlineAuth.type = ce::InlineAuthType::Bearer;
    inlineAuth.token = "{{secret.INLINE_TOKEN}}";
    op.inlineAuth = inlineAuth;

    auto project = makeProject({{"product", {{"get", std::move(op)}}}});
    ce::InlineAuth def;
    def.type = ce::InlineAuthType::Basic;
    def.username = "svc";
    def.password = "{{secret.DEFAULT_PASSWORD}}";
    project.defaultAuth = def;

    const auto names = ce::DependencyResolver::collectSecretReferences(project);
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], "DEFAULT_PASSWORD");
    EXPECT_EQ(names[1], "INLINE_TOKEN");
}

TEST(DependencyResolver, inline_auth_reference_creates_dependency_edge) {
    auto get = makeOp("product", "get");
    ce::InlineAuth inlineAuth;
    inlineAuth.type = ce::InlineAuthType::Bearer;
    inlineAuth.token = "{{session.token}}";
    get.inlineAuth = inlineAuth;
    auto project = makeProject({
        {"session", {{"open", makeOp("session", "open", {jsonExt("token", "$.t")})}}},
        {"product", {{"get", std::move(get)}}},
    });

    ce::DependencyResolver resolver;
    auto chain = resolver.resolve(project, ce::OperationId{"product.get"});
    ASSERT_TRUE(chain.has_value()) << chain.error().detail;
    ASSERT_EQ(chain->size(), 2u);
    EXPECT_EQ((*chain)[0].value, "session.open");
}

TEST(DependencyResolver, inherited_default_auth_reference_creates_dependency_edge) {
    auto get = makeOp("product", "get");
    ce::InlineAuth inherit;
    inherit.type = ce::InlineAuthType::Inherit;
    get.inlineAuth = inherit;
    auto project = makeProject({
        {"session", {{"open", makeOp("session", "open", {jsonExt("token", "$.t")})}}},
        {"product", {{"get", std::move(get)}}},
    });
    ce::InlineAuth def;
    def.type = ce::InlineAuthType::Bearer;
    def.token = "{{session.token}}";
    project.defaultAuth = def;

    ce::DependencyResolver resolver;
    auto chain = resolver.resolve(project, ce::OperationId{"product.get"});
    ASSERT_TRUE(chain.has_value()) << chain.error().detail;
    ASSERT_EQ(chain->size(), 2u);
    EXPECT_EQ((*chain)[0].value, "session.open");
}

TEST(DependencyResolver, poll_until_path_reference_creates_dependency_edge) {
    auto wait = makeOp("report", "wait");
    ce::PollUntil poll;
    poll.pathTemplate = "/api/v1/jobs/{{job.job_id}}";
    poll.successWhen = "$.status == 'done'";
    wait.pollUntil = std::move(poll);
    auto project = makeProject({
        {"job", {{"create", makeOp("job", "create", {jsonExt("job_id", "$.id")})}}},
        {"report", {{"wait", std::move(wait)}}},
    });

    ce::DependencyResolver resolver;
    auto chain = resolver.resolve(project, ce::OperationId{"report.wait"});
    ASSERT_TRUE(chain.has_value()) << chain.error().detail;
    ASSERT_EQ(chain->size(), 2u);
    EXPECT_EQ((*chain)[0].value, "job.create");
}
