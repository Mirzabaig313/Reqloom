// ProjectInsightTest — the read-only project views behind `reqloom describe` and `reqloom plan`
// (PRD FR-13.8, FR-13.9): what an operation consumes and where each value comes from, who depends
// on it, and what a chain needs (sessions, secret names, hosts and their approval) without sending
// anything, reading a secret or writing the approval file.

#include "support/TempPath.h"

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/HostApproval.h>
#include <reqloom/engine/ProjectInsight.h>
#include <reqloom/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace ce = reqloom::engine;
namespace fs = std::filesystem;

namespace {

constexpr const char* kShop = R"YAML(version: 1
name: Shop
id: 11111111-2222-3333-4444-555555555555
environment:
  baseUrl: https://api.shop.test
  admin_password: !secret ADMIN_PASSWORD
  region: eu
actors:
  admin:
    auth:
      strategy: simple
      method: POST
      path: /login
      body:
        password: "{{env.admin_password}}"
      extract:
        token: $.token
    inject:
      headers:
        Authorization: "Bearer {{admin.token}}"
resources:
  order:
    operations:
      create:
        method: POST
        path: /orders
        actor: admin
        headers:
          X-Region: "{{env.region}}"
          X-Api-Key: "{{secret.ORDER_KEY}}"
        extract:
          order_id: $.id
        expect_status: 201
      pay:
        method: POST
        path: /orders/{{order.order_id}}/pay
        actor: admin
        assert:
          - "$.status == \"paid\""
      audit:
        method: GET
        path: /audit
        depends_on: [order.create]
      ghost:
        method: GET
        path: /ghost/{{order.missing_field}}
)YAML";

class ProjectDir {
public:
    ProjectDir() : path_(reqloom::tests::uniqueTempPath("reqloom-insight")) {
        fs::create_directories(path_);
        std::ofstream{path_ / "reqloom.yaml"} << kShop;
    }
    ~ProjectDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    ProjectDir(const ProjectDir&) = delete;
    ProjectDir& operator=(const ProjectDir&) = delete;

    [[nodiscard]] ce::Project load() const {
        auto project = ce::parseProject(path_ / "reqloom.yaml");
        EXPECT_TRUE(project.has_value()) << (project ? "" : project.error().detail);
        return project ? std::move(*project) : ce::Project{};
    }
    [[nodiscard]] const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

[[nodiscard]] const ce::VariableUse* use(const ce::OperationDescription& d, const std::string& t) {
    const auto it = std::ranges::find(d.consumes, t, &ce::VariableUse::token);
    return it == d.consumes.end() ? nullptr : &*it;
}

[[nodiscard]] std::string fileBytes(const fs::path& file) {
    std::ifstream in{file, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

}  // namespace

TEST(ProjectInsight, describe_says_where_each_consumed_value_comes_from) {
    const ProjectDir dir;
    const auto project = dir.load();
    const auto d = ce::describeOperation(project, "order.pay");
    ASSERT_TRUE(d.has_value()) << d.error().detail;
    EXPECT_EQ(d->actor, "admin");
    EXPECT_EQ(d->method, "POST");
    EXPECT_EQ(d->pathTemplate, "/orders/{{order.order_id}}/pay");
    EXPECT_GT(d->source.line, 0);
    const auto* orderId = use(*d, "order.order_id");
    ASSERT_NE(orderId, nullptr);
    EXPECT_EQ(orderId->source, ce::VariableUse::Source::Extraction);
    EXPECT_EQ(orderId->producers, std::vector<std::string>{"order.create"});
    EXPECT_EQ(orderId->usedIn, std::vector<std::string>{"path"});
    EXPECT_EQ(d->dependsOn, std::vector<std::string>{"order.create"});
    ASSERT_EQ(d->assertions.size(), 1U);
}

TEST(ProjectInsight, describe_follows_environment_values_to_secret_names_only) {
    const ProjectDir dir;
    const auto project = dir.load();
    const auto d = ce::describeOperation(project, "order.create");
    ASSERT_TRUE(d.has_value()) << d.error().detail;
    const auto* region = use(*d, "env.region");
    ASSERT_NE(region, nullptr);
    EXPECT_EQ(region->source, ce::VariableUse::Source::Environment);
    EXPECT_TRUE(region->viaSecrets.empty());
    const auto* key = use(*d, "secret.ORDER_KEY");
    ASSERT_NE(key, nullptr);
    EXPECT_EQ(key->source, ce::VariableUse::Source::Secret);
    EXPECT_EQ(key->usedIn, std::vector<std::string>{"headers.X-Api-Key"});
    ASSERT_EQ(d->extracts.size(), 1U);
    EXPECT_EQ(d->extracts[0].variable, "order.order_id");
}

TEST(ProjectInsight, describe_lists_direct_dependents_through_references_and_depends_on) {
    const ProjectDir dir;
    const auto project = dir.load();
    const auto d = ce::describeOperation(project, "order.create");
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->dependents, (std::vector<std::string>{"order.audit", "order.pay"}));
}

TEST(ProjectInsight, describe_refuses_an_unknown_operation) {
    const ProjectDir dir;
    const auto project = dir.load();
    const auto d = ce::describeOperation(project, "order.nope");
    ASSERT_FALSE(d.has_value());
    EXPECT_EQ(d.error().code, ce::ErrorCode::RefUndefined);
}

TEST(ProjectInsight, plan_lists_steps_sessions_and_secret_names_through_the_environment) {
    const ProjectDir dir;
    const auto project = dir.load();
    const auto plan = ce::planChain(project, ce::OperationId{"order.pay"});
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    ASSERT_EQ(plan->steps.size(), 2U);
    EXPECT_EQ(plan->steps[0].step.operationId, "order.create");
    EXPECT_EQ(plan->steps[1].step.operationId, "order.pay");
    EXPECT_TRUE(plan->steps[1].step.isTarget);
    EXPECT_EQ(plan->steps[0].secrets, std::vector<std::string>{"ORDER_KEY"});
    ASSERT_EQ(plan->sessions.size(), 1U);
    EXPECT_EQ(plan->sessions[0].actor, "admin");
    EXPECT_FALSE(plan->sessions[0].cached);
    EXPECT_EQ(plan->sessions[0].secrets, std::vector<std::string>{"ADMIN_PASSWORD"});
    EXPECT_EQ(plan->secrets, (std::vector<std::string>{"ADMIN_PASSWORD", "ORDER_KEY"}));
}

TEST(ProjectInsight, plan_reports_a_session_already_in_the_context_as_cached) {
    const ProjectDir dir;
    const auto project = dir.load();
    ce::RunContext ctx;
    ce::ActorSession live;
    live.state = ce::ActorSession::State::Live;
    ctx.putSession(ce::ActorId{"admin"}, live);
    ce::PlanOptions options;
    options.context = &ctx;
    const auto plan = ce::planChain(project, ce::OperationId{"order.pay"}, options);
    ASSERT_TRUE(plan.has_value());
    ASSERT_EQ(plan->sessions.size(), 1U);
    EXPECT_TRUE(plan->sessions[0].cached);
}

TEST(ProjectInsight, plan_judges_hosts_without_approving_or_writing_anything) {
    const ProjectDir dir;
    const auto project = dir.load();
    const auto trustFile = dir.path() / "trust" / "projects.json";
    fs::create_directories(trustFile.parent_path());
    auto store = ce::makeFileHostApprovalStore(trustFile);

    ce::PlanOptions options;
    options.approvals = store.get();
    auto plan = ce::planChain(project, ce::OperationId{"order.pay"}, options);
    ASSERT_TRUE(plan.has_value());
    ASSERT_EQ(plan->hosts.size(), 1U);
    EXPECT_EQ(plan->hosts[0].host, "https://api.shop.test:443");
    EXPECT_EQ(plan->hosts[0].status, ce::HostStatus::NotApproved);
    EXPECT_FALSE(fs::exists(trustFile)) << "plan must not create the approval file";

    ASSERT_TRUE(store->approve(project, "https://api.shop.test:443").has_value());
    const auto before = fileBytes(trustFile);
    plan = ce::planChain(project, ce::OperationId{"order.pay"}, options);
    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->hosts[0].status, ce::HostStatus::Approved);
    EXPECT_EQ(fileBytes(trustFile), before) << "plan must not write the approval file";

    options.approvals = nullptr;
    options.approvedForRun = {"https://api.shop.test:443"};
    plan = ce::planChain(project, ce::OperationId{"order.pay"}, options);
    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->hosts[0].status, ce::HostStatus::ApprovedForRun);
}

TEST(ProjectInsight, plan_refuses_a_host_outside_the_actors_allowed_hosts) {
    const ProjectDir dir;
    auto project = dir.load();
    project.actors.at(ce::ActorId{"admin"}).allowedHosts = {"auth.shop.test"};
    const auto plan = ce::planChain(project, ce::OperationId{"order.create"});
    ASSERT_TRUE(plan.has_value());
    ASSERT_FALSE(plan->hosts.empty());
    EXPECT_EQ(plan->hosts[0].status, ce::HostStatus::Refused);
    EXPECT_NE(plan->hosts[0].detail.find("allowed_hosts"), std::string::npos);
}

TEST(ProjectInsight, plan_marks_a_templated_host_unknown) {
    const ProjectDir dir;
    auto project = dir.load();
    project.environments.at(project.defaultEnvironment)["baseUrl"] = "https://{{env.region}}.shop";
    const auto plan = ce::planChain(project, ce::OperationId{"order.create"});
    ASSERT_TRUE(plan.has_value());
    ASSERT_EQ(plan->hosts.size(), 1U);
    EXPECT_EQ(plan->hosts[0].status, ce::HostStatus::Unknown);
    EXPECT_TRUE(plan->hosts[0].host.empty());
}

TEST(ProjectInsight, a_reference_nothing_extracts_is_found_with_its_line_and_a_fix) {
    const ProjectDir dir;
    const auto project = dir.load();
    const auto findings = ce::unproducedReferences(project);
    ASSERT_EQ(findings.size(), 1U);
    EXPECT_EQ(findings[0].reference, "{{order.missing_field}}");
    EXPECT_EQ(findings[0].file, (dir.path() / "reqloom.yaml").string());
    EXPECT_EQ(findings[0].line, 46);  // `path:` of order.ghost
    EXPECT_NE(findings[0].fix.find("order_id"), std::string::npos) << findings[0].fix;
}

TEST(ProjectInsight, describe_finds_the_producer_also_named_in_depends_on) {
    const ProjectDir dir;
    auto project = dir.load();
    auto& pay = project.resources.at(ce::ResourceId{"order"}).operations.at("pay");
    pay.explicitDependencies = {ce::OperationId{"order.create"}};
    const auto d = ce::describeOperation(project, "order.pay");
    ASSERT_TRUE(d.has_value());
    const auto* orderId = use(*d, "order.order_id");
    ASSERT_NE(orderId, nullptr);
    EXPECT_EQ(orderId->producers, std::vector<std::string>{"order.create"});
}

TEST(ProjectInsight, plan_lists_the_inline_oauth2_token_host) {
    const ProjectDir dir;
    auto project = dir.load();
    auto& audit = project.resources.at(ce::ResourceId{"order"}).operations.at("audit");
    ce::InlineAuth auth;
    auth.type = ce::InlineAuthType::OAuth2;
    auth.oauth2GrantType = "client_credentials";
    auth.oauth2TokenUrl = "https://auth.shop.test/oauth/token";
    auth.oauth2ClientSecret = "{{secret.CLIENT_SECRET}}";
    audit.inlineAuth = auth;
    const auto plan = ce::planChain(project, ce::OperationId{"order.audit"});
    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    const auto it =
        std::ranges::find(plan->hosts, "https://auth.shop.test:443", &ce::PlannedHost::host);
    ASSERT_NE(it, plan->hosts.end());
    EXPECT_EQ(it->status, ce::HostStatus::NotApproved);
    EXPECT_EQ(it->usedBy, std::vector<std::string>{"token request of order.audit"});
}

TEST(ProjectInsight, an_unproduced_reference_in_inherited_default_auth_points_at_the_default) {
    const ProjectDir dir;
    auto project = dir.load();
    ce::InlineAuth auth;
    auth.type = ce::InlineAuthType::Bearer;
    auth.token = "{{order.never_extracted}}";
    project.defaultAuth = auth;
    project.defaultAuthSource = ce::SourceLocation{"root.yaml", 4, 1, {}};
    ce::InlineAuth inherit;
    inherit.type = ce::InlineAuthType::Inherit;
    project.resources.at(ce::ResourceId{"order"}).operations.at("audit").inlineAuth = inherit;
    const auto findings = ce::unproducedReferences(project);
    const auto it =
        std::ranges::find(findings, "{{order.never_extracted}}", &ce::ReqloomError::reference);
    ASSERT_NE(it, findings.end());
    EXPECT_EQ(it->file, "root.yaml");
    EXPECT_EQ(it->line, 4);
}
