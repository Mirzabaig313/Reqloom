// Tests the pure helpers behind ProjectModel's structural edits: name rules against the engine,
// reference rewriting, extraction and auth-step merging, environment choice and producer lookup.
#include "application/ProjectEdits.h"

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace reqloom::desktop::tests {

namespace {

namespace ce = reqloom::engine;

[[nodiscard]] bool engineAcceptsEnvironmentName(const std::string& name) {
    ce::Project project;
    project.environments[name] = {};
    return ce::validateProject(project).has_value();
}

/// users.create extracts id; orders.list consumes {{users.id}}.
ce::Project producerProject() {
    ce::Project p;
    ce::Resource users;
    users.id = ce::ResourceId{"users"};
    ce::Operation create;
    create.id = ce::OperationId{"users.create"};
    create.resource = users.id;
    create.method = ce::HttpMethod::Post;
    create.pathTemplate = "/users";
    create.extractions.push_back({"id", "$.id", ce::Extraction::Source::JsonPath});
    users.operations["create"] = create;
    p.resources[users.id] = users;

    ce::Resource orders;
    orders.id = ce::ResourceId{"orders"};
    ce::Operation list;
    list.id = ce::OperationId{"orders.list"};
    list.resource = orders.id;
    list.pathTemplate = "/orders/{{users.id}}";
    orders.operations["list"] = list;
    p.resources[orders.id] = orders;
    return p;
}

}  // namespace

TEST(ProjectEdits, file_stem_rule_agrees_with_engine) {
    const std::vector<std::string> names{"users",
                                         "Users",
                                         "a-b_c",
                                         "",
                                         ".hidden",
                                         "a/b",
                                         "a\\b",
                                         "c:d",
                                         std::string{"nu\0l", 4},
                                         "a.b",
                                         "trailing.",
                                         "spaced name",
                                         "ünïcode"};
    for (const auto& name : names) {
        EXPECT_EQ(isSafeFileStem(name), engineAcceptsEnvironmentName(name)) << name;
        if (isValidEntityName(name)) {
            EXPECT_TRUE(engineAcceptsEnvironmentName(name)) << name;
        }
    }
    EXPECT_FALSE(isValidEntityName("c:d"));
    EXPECT_FALSE(isValidEntityName(std::string{"nu\0l", 4}));
    EXPECT_FALSE(isValidEntityName("a.b"));
    EXPECT_TRUE(isValidEntityName("users"));
}

TEST(ProjectEdits, case_insensitive_clash_names_existing_entry_but_not_self) {
    const std::vector<std::string> existing{"users", "orders"};
    EXPECT_EQ(caseInsensitiveClash(existing, "Users"), std::optional<std::string>{"users"});
    EXPECT_EQ(caseInsensitiveClash(existing, "Users", "users"), std::nullopt);
    EXPECT_EQ(caseInsensitiveClash(existing, "people"), std::nullopt);
}

TEST(ProjectEdits, rename_reference_scope_rewrites_only_placeholders) {
    struct Case {
        std::string_view in;
        std::string_view out;
    };
    const std::vector<Case> cases{
        {"/u/{{users.id}}", "/u/{{people.id}}"},
        {"/u/{{ users.id }}", "/u/{{ people.id }}"},
        {"{{\tusers.items[0].id}}", "{{\tpeople.items[0].id}}"},
        {"{{users[0].id}}", "{{people[0].id}}"},
        {"{{$.base64(users.id)}}", "{{$.base64(people.id)}}"},
        {"{{ $.base64( users.id ) }}", "{{ $.base64( people.id ) }}"},
        {"{{$.base64('users.id')}}", "{{$.base64('users.id')}}"},
        {"users.id and {{usersx.id}} and {{user.id}}",
         "users.id and {{usersx.id}} and {{user.id}}"},
        {"{{users}}", "{{users}}"},
        {"{{env.users}}", "{{env.users}}"},
        {"{{users.a}}{{users.b}}", "{{people.a}}{{people.b}}"},
        {"{{users.a}b}} {{users.c}}", "{{users.a}b}} {{people.c}}"},
        {"{{ unterminated users.id", "{{ unterminated users.id"},
    };
    for (const auto& c : cases) {
        EXPECT_EQ(renameReferenceScope(c.in, "users", "people"), c.out) << c.in;
    }
}

TEST(ProjectEdits, find_scope_references_reports_owner_and_reference) {
    const auto refs = findScopeReferences(producerProject(), "users");
    ASSERT_EQ(refs.size(), 1U);
    EXPECT_EQ(refs[0].owner, "orders.list");
    EXPECT_EQ(refs[0].what, "{{users.id}}");
    EXPECT_TRUE(findScopeReferences(producerProject(), "users", std::string_view{"name"}).empty());
}

TEST(ProjectEdits, merge_extractions_keeps_existing_source_and_infers_new_ones) {
    const std::vector<ce::Extraction> existing{
        {"token", "X-Token", ce::Extraction::Source::Header},
        {"sid", "sid=([a-z]+)", ce::Extraction::Source::Regex},
        {"id", "$.id", ce::Extraction::Source::JsonPath},
    };
    const std::vector<std::pair<QString, QString>> rows{
        {QStringLiteral("token"), QStringLiteral("X-Auth")},
        {QStringLiteral("sid"), QStringLiteral("sid=([a-z]+)")},
        {QStringLiteral("id"), QStringLiteral("$.headers.X-Id")},
        {QStringLiteral("code"), QStringLiteral("$.status_code")},
        {QStringLiteral("jar"), QStringLiteral("$.cookies.session")},
        {QString{}, QStringLiteral("$.ignored")},
    };

    const auto merged = mergeExtractions(existing, rows);

    ASSERT_EQ(merged.size(), 5U);
    EXPECT_EQ(merged[0].source, ce::Extraction::Source::Header);
    EXPECT_EQ(merged[0].sourcePath, "X-Auth");
    EXPECT_EQ(merged[1].source, ce::Extraction::Source::Regex);
    EXPECT_EQ(merged[2].source, ce::Extraction::Source::Header);
    EXPECT_EQ(merged[3].source, ce::Extraction::Source::StatusCode);
    EXPECT_EQ(merged[4].source, ce::Extraction::Source::Cookie);
}

TEST(ProjectEdits, merge_auth_steps_preserves_fields_the_editor_does_not_show) {
    ce::AuthStep login;
    login.id = "login";
    login.pathTemplate = "/login";
    login.headers["X-Client"] = "desktop";
    login.extractions.push_back({"token", "X-Token", ce::Extraction::Source::Header});
    ce::AuthStep mfa;
    mfa.id = "mfa";
    mfa.headers["X-Mfa"] = "1";

    AuthStepEdit editLogin;
    editLogin.id = "login";
    editLogin.pathTemplate = "/v2/login";
    editLogin.extractions = {{QStringLiteral("token"), QStringLiteral("X-Token")}};
    AuthStepEdit renamedMfa;
    renamedMfa.id = "otp";
    AuthStepEdit added;
    added.id = "extra";

    const auto merged = mergeAuthSteps({login, mfa}, {editLogin, renamedMfa, added});

    ASSERT_EQ(merged.size(), 3U);
    EXPECT_EQ(merged[0].pathTemplate, "/v2/login");
    EXPECT_EQ(merged[0].headers.at("X-Client"), "desktop");
    ASSERT_EQ(merged[0].extractions.size(), 1U);
    EXPECT_EQ(merged[0].extractions[0].source, ce::Extraction::Source::Header);
    EXPECT_EQ(merged[1].id, "otp");
    EXPECT_EQ(merged[1].headers.at("X-Mfa"), "1");
    EXPECT_TRUE(merged[2].headers.empty());
}

TEST(ProjectEdits, choose_environment_prefers_saved_then_default_then_first) {
    const QStringList envs{QStringLiteral("local"), QStringLiteral("prod")};
    EXPECT_EQ(chooseEnvironment(envs, QStringLiteral("prod"), QStringLiteral("local")),
              QStringLiteral("prod"));
    EXPECT_EQ(chooseEnvironment(envs, QStringLiteral("gone"), QStringLiteral("local")),
              QStringLiteral("local"));
    EXPECT_EQ(chooseEnvironment(envs, QString{}, QStringLiteral("gone")), QStringLiteral("local"));
    EXPECT_EQ(chooseEnvironment({}, QStringLiteral("prod"), QStringLiteral("prod")), QString{});
}

TEST(ProjectEdits, producer_from_plan_uses_engine_rule_and_reports_ambiguity) {
    auto engineInstance = ce::ExecutionEngine{ce::makeDefaultDependencies()};
    ce::Project project = producerProject();
    const ce::OperationId target{"orders.list"};

    const auto found =
        producerFromPlan(engineInstance.resolvePlan(project, target), project, target, "users.id");
    EXPECT_EQ(found.producer, "users.create");
    EXPECT_EQ(found.sourcePath, "$.id");
    EXPECT_TRUE(found.problem.isEmpty());

    const auto bare =
        producerFromPlan(engineInstance.resolvePlan(project, target), project, target, "id");
    EXPECT_TRUE(bare.producer.empty());
    EXPECT_FALSE(bare.problem.isEmpty());

    // A second extractor of users.id makes the producer ambiguous without depends_on.
    ce::Operation search = project.resources.at(ce::ResourceId{"users"}).operations.at("create");
    search.id = ce::OperationId{"users.search"};
    project.resources.at(ce::ResourceId{"users"}).operations["search"] = search;
    const auto ambiguous =
        producerFromPlan(engineInstance.resolvePlan(project, target), project, target, "users.id");
    EXPECT_TRUE(ambiguous.producer.empty());
    EXPECT_TRUE(ambiguous.problem.contains(QStringLiteral("ambiguous")));
}

}  // namespace reqloom::desktop::tests
