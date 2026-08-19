// ExecutionPreview — the resolved execution path shown before a run.

#include <gtest/gtest.h>

#include "application/ExecutionPreview.h"

#include <reqloom/engine/Factories.h>

#include <filesystem>

namespace reqloom::desktop::tests {

namespace {

/// Add an operation to `project`, creating its resource on first use. `id` is
/// "<resource>.<name>".
engine::Operation& addOperation(engine::Project& project,
                                const std::string& id,
                                engine::HttpMethod method,
                                const std::string& pathTemplate) {
    const std::size_t dot = id.rfind('.');
    const engine::ResourceId resourceId{id.substr(0, dot)};
    engine::Resource& resource = project.resources[resourceId];
    resource.id = resourceId;

    engine::Operation& op = resource.operations[id.substr(dot + 1)];
    op.id = engine::OperationId{id};
    op.resource = resourceId;
    op.method = method;
    op.pathTemplate = pathTemplate;
    return op;
}

/// auth.login → order.create, where the second consumes a token the first
/// extracts. Mirrors the shape resolvePlan returns for a real chain.
engine::Project twoStepProject() {
    engine::Project project;
    engine::Operation& login =
        addOperation(project, "auth.login", engine::HttpMethod::Post, "/oauth/token");
    // Bare, exactly as YamlSchemaParser stores an `extract:` key. The resource
    // prefix is applied downstream; a fixture that pre-qualifies it would hide
    // the very mismatch these tests exist to pin down.
    login.extractions.push_back(
        engine::Extraction{.variableName = "token", .sourcePath = "$.access_token"});

    engine::Operation& create =
        addOperation(project, "order.create", engine::HttpMethod::Post, "/orders/{{auth.token}}");
    create.actor = engine::ActorId{"buyer"};
    create.expectStatusList = {200, 202};
    return project;
}

engine::ResolvedPlan twoStepPlan() {
    engine::ResolvedPlan plan;
    plan.order = {engine::OperationId{"auth.login"}, engine::OperationId{"order.create"}};
    plan.edges.push_back(engine::DependencyEdge{.consumer = engine::OperationId{"order.create"},
                                                .producer = engine::OperationId{"auth.login"},
                                                .implicit = true,
                                                .variable = "auth.token"});
    return plan;
}

}  // namespace

TEST(ExecutionPreview, orders_steps_dependencies_first_and_marks_the_target_last) {
    const auto steps = buildExecutionPreview(twoStepPlan(), twoStepProject());

    ASSERT_EQ(steps.size(), 2u);
    EXPECT_EQ(steps[0].number, 1);
    EXPECT_EQ(steps[0].operationId, QStringLiteral("auth.login"));
    EXPECT_FALSE(steps[0].isTarget);
    EXPECT_EQ(steps[1].number, 2);
    EXPECT_EQ(steps[1].operationId, QStringLiteral("order.create"));
    EXPECT_TRUE(steps[1].isTarget);
}

TEST(ExecutionPreview, reports_method_actor_and_expected_statuses_from_the_project) {
    const auto steps = buildExecutionPreview(twoStepPlan(), twoStepProject());

    ASSERT_EQ(steps.size(), 2u);
    EXPECT_EQ(steps[0].method, QStringLiteral("POST"));
    EXPECT_EQ(steps[1].actor, QStringLiteral("buyer"));
    EXPECT_EQ(steps[1].expectStatus, QList<int>({200, 202}));
    // Unset on step one, so nothing is invented.
    EXPECT_TRUE(steps[0].expectStatus.isEmpty());
}

TEST(ExecutionPreview, leaves_variable_references_unresolved_in_paths) {
    const auto steps = buildExecutionPreview(twoStepPlan(), twoStepProject());

    ASSERT_EQ(steps.size(), 2u);
    // An unresolved template cannot leak a resolved secret into the preview.
    EXPECT_EQ(steps[1].pathTemplate, QStringLiteral("/orders/{{auth.token}}"));
}

TEST(ExecutionPreview, lists_produced_variables_with_their_source_paths) {
    const auto steps = buildExecutionPreview(twoStepPlan(), twoStepProject());

    ASSERT_EQ(steps.size(), 2u);
    ASSERT_EQ(steps[0].produces.size(), 1u);
    EXPECT_EQ(steps[0].produces[0].variable, QStringLiteral("auth.token"));
    EXPECT_EQ(steps[0].produces[0].sourcePath, QStringLiteral("$.access_token"));
    EXPECT_TRUE(steps[1].produces.empty());
}

// A dotted extraction key is still prefixed. Real usage: `extract:
// { employer.org_id: ... }` on admin_organization.list stores the literal key
// "employer.org_id" under resource `admin_organization`. Because a reference is
// split at its FIRST dot, the only reference that reaches it is
// `{{admin_organization.employer.org_id}}` — NOT `{{employer.org_id}}`, which
// addresses an `employer` actor session instead.
TEST(ExecutionPreview, prefixes_a_dotted_extraction_key_because_refs_split_on_the_first_dot) {
    engine::Project project;
    engine::Operation& op = addOperation(
        project, "admin_organization.list", engine::HttpMethod::Get, "/admin/organizations");
    op.extractions.push_back(
        engine::Extraction{.variableName = "employer.org_id", .sourcePath = "$.data.items[0].id"});
    engine::ResolvedPlan plan;
    plan.order = {engine::OperationId{"admin_organization.list"}};

    const auto steps = buildExecutionPreview(plan, project);

    ASSERT_EQ(steps.size(), 1u);
    ASSERT_EQ(steps[0].produces.size(), 1u);
    EXPECT_EQ(steps[0].produces[0].variable, QStringLiteral("admin_organization.employer.org_id"));
}

// The schema stores extraction names bare; templates and edges use the qualified
// form. Reporting the bare name would print a variable that fails if copied.
TEST(ExecutionPreview, qualifies_produced_names_so_they_match_how_templates_reference_them) {
    engine::Project project;
    engine::Operation& op =
        addOperation(project, "orders.create", engine::HttpMethod::Post, "/orders");
    op.extractions.push_back(
        engine::Extraction{.variableName = "order_id", .sourcePath = "$.data.id"});
    engine::ResolvedPlan plan;
    plan.order = {engine::OperationId{"orders.create"}};

    const auto steps = buildExecutionPreview(plan, project);

    ASSERT_EQ(steps.size(), 1u);
    ASSERT_EQ(steps[0].produces.size(), 1u);
    // Stored as "order_id"; must display as "orders.order_id".
    EXPECT_EQ(steps[0].produces[0].variable, QStringLiteral("orders.order_id"));
}

TEST(ExecutionPreview, names_the_steps_that_consume_a_produced_variable) {
    const auto consumers =
        consumersOfVariable(twoStepPlan(), QStringLiteral("auth.login"), QStringLiteral("token"));

    EXPECT_EQ(consumers, QStringList({QStringLiteral("order.create")}));
}

TEST(ExecutionPreview, accepts_a_variable_name_in_either_bare_or_qualified_form) {
    const auto bare =
        consumersOfVariable(twoStepPlan(), QStringLiteral("auth.login"), QStringLiteral("token"));
    const auto qualified = consumersOfVariable(
        twoStepPlan(), QStringLiteral("auth.login"), QStringLiteral("auth.token"));

    // Extraction events report the bare form, the preview the qualified one.
    // Both must resolve, or a caller silently gets no consumers.
    EXPECT_EQ(bare, qualified);
    EXPECT_FALSE(bare.isEmpty());
}

TEST(ExecutionPreview, finds_consumers_of_a_dotted_extraction_key) {
    engine::ResolvedPlan plan;
    plan.order = {engine::OperationId{"admin_organization.list"},
                  engine::OperationId{"admin_organization.get"}};
    // DependencyResolver builds the edge variable as refResource + "." + refVar,
    // so a `{{admin_organization.employer.org_id}}` reference yields this.
    plan.edges.push_back(
        engine::DependencyEdge{.consumer = engine::OperationId{"admin_organization.get"},
                               .producer = engine::OperationId{"admin_organization.list"},
                               .implicit = true,
                               .variable = "admin_organization.employer.org_id"});

    // The extraction event reports the stored key, dots and all.
    const auto consumers = consumersOfVariable(
        plan, QStringLiteral("admin_organization.list"), QStringLiteral("employer.org_id"));

    EXPECT_EQ(consumers, QStringList({QStringLiteral("admin_organization.get")}));
}

TEST(ExecutionPreview, reports_no_consumers_for_a_variable_nobody_uses) {
    const auto consumers = consumersOfVariable(
        twoStepPlan(), QStringLiteral("auth.login"), QStringLiteral("unused_value"));

    EXPECT_TRUE(consumers.isEmpty());
}

TEST(ExecutionPreview, does_not_credit_an_explicit_dependency_as_a_variable_consumer) {
    engine::ResolvedPlan plan;
    plan.order = {engine::OperationId{"auth.login"}, engine::OperationId{"order.create"}};
    // depends_on only: ordering, with no value flowing along it.
    plan.edges.push_back(engine::DependencyEdge{.consumer = engine::OperationId{"order.create"},
                                                .producer = engine::OperationId{"auth.login"},
                                                .implicit = false,
                                                .variable = {}});

    const auto consumers =
        consumersOfVariable(plan, QStringLiteral("auth.login"), QStringLiteral("token"));

    // order.create runs after auth.login but never reads the token, so a missing
    // token is not evidence that order.create broke because of it.
    EXPECT_TRUE(consumers.isEmpty());
}

TEST(ExecutionPreview, lists_every_consumer_of_a_shared_variable_once_and_sorted) {
    engine::ResolvedPlan plan = twoStepPlan();
    plan.order.push_back(engine::OperationId{"audit.record"});
    for (const char* consumer : {"audit.record", "order.create"}) {
        plan.edges.push_back(engine::DependencyEdge{.consumer = engine::OperationId{consumer},
                                                    .producer = engine::OperationId{"auth.login"},
                                                    .implicit = true,
                                                    .variable = "auth.token"});
    }

    const auto consumers =
        consumersOfVariable(plan, QStringLiteral("auth.login"), QStringLiteral("token"));

    EXPECT_EQ(consumers,
              QStringList({QStringLiteral("audit.record"), QStringLiteral("order.create")}));
}

TEST(ExecutionPreview, attributes_dependencies_to_the_consuming_step) {
    const auto steps = buildExecutionPreview(twoStepPlan(), twoStepProject());

    ASSERT_EQ(steps.size(), 2u);
    EXPECT_TRUE(steps[0].dependsOn.isEmpty());
    EXPECT_EQ(steps[1].dependsOn, QStringList({QStringLiteral("auth.login")}));
}

TEST(ExecutionPreview, collapses_explicit_and_implicit_edges_between_one_pair) {
    engine::ResolvedPlan plan = twoStepPlan();
    // The same pair also declared via depends_on: one dependency to a reader.
    plan.edges.push_back(engine::DependencyEdge{.consumer = engine::OperationId{"order.create"},
                                                .producer = engine::OperationId{"auth.login"},
                                                .implicit = false,
                                                .variable = {}});

    const auto steps = buildExecutionPreview(plan, twoStepProject());

    ASSERT_EQ(steps.size(), 2u);
    EXPECT_EQ(steps[1].dependsOn, QStringList({QStringLiteral("auth.login")}));
}

TEST(ExecutionPreview, previews_a_dependency_free_operation_as_a_single_step) {
    engine::Project project;
    addOperation(project, "health.check", engine::HttpMethod::Get, "/healthz");
    engine::ResolvedPlan plan;
    plan.order = {engine::OperationId{"health.check"}};

    const auto steps = buildExecutionPreview(plan, project);

    // The chain graph deliberately draws nothing for a one-step plan; the
    // preview must still describe the single request that will be sent.
    ASSERT_EQ(steps.size(), 1u);
    EXPECT_EQ(steps[0].method, QStringLiteral("GET"));
    EXPECT_EQ(steps[0].pathTemplate, QStringLiteral("/healthz"));
    EXPECT_TRUE(steps[0].isTarget);
}

TEST(ExecutionPreview, keeps_a_step_whose_operation_is_missing_from_the_project) {
    engine::ResolvedPlan plan;
    plan.order = {engine::OperationId{"ghost.gone"}};

    const auto steps = buildExecutionPreview(plan, engine::Project{});

    // Hiding it would make a broken chain look shorter than it is.
    ASSERT_EQ(steps.size(), 1u);
    EXPECT_EQ(steps[0].operationId, QStringLiteral("ghost.gone"));
    EXPECT_TRUE(steps[0].method.isEmpty());
    EXPECT_TRUE(steps[0].pathTemplate.isEmpty());
}

TEST(ExecutionPreview, returns_no_steps_for_an_empty_plan) {
    EXPECT_TRUE(buildExecutionPreview(engine::ResolvedPlan{}, engine::Project{}).empty());
}

// Against the real sample project rather than hand-built structs: catches a
// drift between what the engine's plan actually contains and what the preview
// assumes about it, which unit structs cannot.
TEST(ExecutionPreview, previews_every_operation_in_the_marketplace_sample) {
    const auto project = engine::parseProject(std::filesystem::path{REQLOOM_SAMPLES_DIR} /
                                              "marketplace" / "reqloom.yaml");
    ASSERT_TRUE(project.has_value());

    auto engineInstance = engine::ExecutionEngine{engine::makeDefaultDependencies()};

    int previewed = 0;
    int withDependencies = 0;
    int withOutputs = 0;

    for (const auto& [resourceId, resource] : project->resources) {
        for (const auto& [opName, op] : resource.operations) {
            const auto plan = engineInstance.resolvePlan(*project, op.id);
            ASSERT_TRUE(plan.has_value()) << "unresolvable: " << op.id.value;

            const auto steps = buildExecutionPreview(*plan, *project);
            ASSERT_EQ(steps.size(), plan->order.size());
            ++previewed;

            // Numbering is dense and 1-based, and the requested operation is last.
            for (std::size_t i = 0; i < steps.size(); ++i) {
                EXPECT_EQ(steps[i].number, static_cast<int>(i) + 1);
            }
            EXPECT_TRUE(steps.back().isTarget);
            EXPECT_EQ(steps.back().operationId, QString::fromStdString(op.id.value));

            // Every step resolved against the project, so no id came back unknown.
            for (const auto& step : steps) {
                EXPECT_FALSE(step.method.isEmpty()) << step.operationId.toStdString();
            }

            if (steps.size() > 1) {
                ++withDependencies;
            }
            for (const auto& step : steps) {
                if (!step.produces.empty()) {
                    ++withOutputs;
                    break;
                }
            }
        }
    }

    EXPECT_GT(previewed, 0);
    // The sample is a chained project; if these ever hit zero the sample changed
    // shape and this test stopped covering the case it was written for.
    EXPECT_GT(withDependencies, 0);
    EXPECT_GT(withOutputs, 0);
}

}  // namespace reqloom::desktop::tests
