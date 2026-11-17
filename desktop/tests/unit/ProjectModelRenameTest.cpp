// Tests ProjectModel renames and deletes through a real write→reload cycle: names that differ only
// in case, references that follow a rename, and deletes refused while something still refers.
#include "application/ProjectModel.h"

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <QtCore/QTemporaryDir>

#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace reqloom::desktop::tests {

namespace {

namespace ce = reqloom::engine;
namespace fs = std::filesystem;

ce::Operation makeOp(const std::string& resource, const std::string& name, std::string path) {
    ce::Operation op;
    op.id = ce::OperationId{resource + "." + name};
    op.resource = ce::ResourceId{resource};
    op.actor = ce::ActorId{"user"};
    op.pathTemplate = std::move(path);
    return op;
}

/// Actor `user`; `users.create` extracts id; `orders.list` refers to users and the actor in every
/// reference form; `orders.each` fans out over users; `audit.log` depends on users.create.
ce::Project buildReferenceProject() {
    ce::Project p;
    p.name = "Refs";
    p.defaultEnvironment = "local";
    p.environments["local"] = {{"baseUrl", "http://localhost:0"}, {"who", "{{users.id}}"}};

    ce::Actor user;
    user.id = ce::ActorId{"user"};
    ce::AuthStep step;
    step.id = "login";
    step.pathTemplate = "/login";
    step.extractions.push_back({"token", "$.token", ce::Extraction::Source::JsonPath});
    user.authSteps.push_back(std::move(step));
    user.inject.headers["Authorization"] = "Bearer {{user.token}}";
    p.actors[user.id] = std::move(user);

    ce::Resource users;
    users.id = ce::ResourceId{"users"};
    auto create = makeOp("users", "create", "/users");
    create.method = ce::HttpMethod::Post;
    create.extractions.push_back({"id", "$.id", ce::Extraction::Source::JsonPath});
    users.operations["create"] = std::move(create);
    p.resources[users.id] = std::move(users);

    ce::Resource orders;
    orders.id = ce::ResourceId{"orders"};
    auto list = makeOp("orders", "list", "/orders/{{ users.id }}");
    list.headers["X-Note"] = "users.id";
    list.headers["X-Token"] = "{{user.token}}";
    list.queryParams["h"] = "{{$.base64(users.id)}}";
    ce::PollUntil poll;
    poll.pathTemplate = "/status/{{users.id}}";
    poll.actor = ce::ActorId{"user"};
    poll.successWhen = "$.status == 'done'";
    list.pollUntil = poll;
    orders.operations["list"] = std::move(list);
    auto each = makeOp("orders", "each", "/orders");
    each.forEach = ce::ForEach{ce::ResourceId{"users"}};
    orders.operations["each"] = std::move(each);
    p.resources[orders.id] = std::move(orders);

    ce::Resource audit;
    audit.id = ce::ResourceId{"audit"};
    auto log = makeOp("audit", "log", "/audit");
    log.explicitDependencies = {ce::OperationId{"users.create"}};
    audit.operations["log"] = std::move(log);
    p.resources[audit.id] = std::move(audit);
    return p;
}

[[nodiscard]] bool load(const QTemporaryDir& dir, const ce::Project& project, ProjectModel& model) {
    if (!ce::writeProject(fs::path{dir.path().toStdString()}, project, true).has_value()) {
        return false;
    }
    model.loadFromDirectory(dir.path());
    return model.hasProject();
}

[[nodiscard]] std::set<std::string> yamlFilesIn(const fs::path& folder) {
    std::set<std::string> names;
    for (const auto& entry : fs::directory_iterator(folder)) {
        if (entry.path().extension() == ".yaml") {
            names.insert(entry.path().filename().string());
        }
    }
    return names;
}

[[nodiscard]] bool volumeIsCaseInsensitive(const fs::path& folder) {
    const auto probe = folder / "case-probe";
    std::ofstream{probe} << "x";
    std::error_code ec;
    const bool insensitive = fs::equivalent(probe, folder / "CASE-PROBE", ec) && !ec;
    fs::remove(probe, ec);
    return insensitive;
}

[[nodiscard]] const ce::Operation& opIn(const ProjectModel& model, const std::string& id) {
    const auto* op = model.findOperation(ce::OperationId{id});
    if (op == nullptr) {
        throw std::runtime_error{"missing operation " + id};
    }
    return *op;
}

}  // namespace

TEST(ProjectModelRename, case_only_resource_rename_keeps_the_module_on_disk) {
    QTemporaryDir dir;
    ProjectModel model;
    ASSERT_TRUE(load(dir, buildReferenceProject(), model));
    const fs::path root{dir.path().toStdString()};

    QString error;
    ASSERT_TRUE(model.renameResource(ce::ResourceId{"users"}, QStringLiteral("Users"), error))
        << error.toStdString();

    ProjectModel reloaded;
    reloaded.loadFromDirectory(dir.path());
    ASSERT_TRUE(reloaded.hasProject());
    EXPECT_NE(reloaded.findOperation(ce::OperationId{"Users.create"}), nullptr);
    EXPECT_EQ(opIn(reloaded, "orders.list").pathTemplate, "/orders/{{ Users.id }}");

    if (!volumeIsCaseInsensitive(root)) {
        GTEST_SKIP()
            << "case-sensitive volume: the on-disk name check needs a case-insensitive one";
    }
    const auto files = yamlFilesIn(root / "resources");
    EXPECT_TRUE(files.contains("Users.yaml"));
    EXPECT_FALSE(files.contains("users.yaml"));
}

TEST(ProjectModelRename, case_only_actor_and_environment_renames_keep_their_files) {
    QTemporaryDir dir;
    ProjectModel model;
    ASSERT_TRUE(load(dir, buildReferenceProject(), model));
    const fs::path root{dir.path().toStdString()};

    QString error;
    ce::Actor renamed = model.project().actors.at(ce::ActorId{"user"});
    renamed.id = ce::ActorId{"User"};
    ASSERT_TRUE(model.saveActor(QStringLiteral("user"), renamed, error)) << error.toStdString();
    ASSERT_TRUE(model.saveEnvironment(QStringLiteral("local"),
                                      QStringLiteral("Local"),
                                      model.project().environments.at("local"),
                                      error))
        << error.toStdString();

    ProjectModel reloaded;
    reloaded.loadFromDirectory(dir.path());
    ASSERT_TRUE(reloaded.hasProject());
    EXPECT_TRUE(reloaded.project().actors.contains(ce::ActorId{"User"}));
    EXPECT_TRUE(reloaded.project().environments.contains("Local"));
    EXPECT_EQ(reloaded.project().defaultEnvironment, "Local");

    if (!volumeIsCaseInsensitive(root)) {
        GTEST_SKIP()
            << "case-sensitive volume: the on-disk name check needs a case-insensitive one";
    }
    EXPECT_EQ(yamlFilesIn(root / "actors"), std::set<std::string>{"User.yaml"});
    EXPECT_EQ(yamlFilesIn(root / "environments"), std::set<std::string>{"Local.yaml"});
}

TEST(ProjectModelRename, names_that_differ_only_in_case_are_rejected) {
    QTemporaryDir dir;
    ProjectModel model;
    ASSERT_TRUE(load(dir, buildReferenceProject(), model));

    QString error;
    EXPECT_FALSE(model.createResource(QStringLiteral("USERS"), {}, error));
    EXPECT_TRUE(error.contains(QStringLiteral("users"))) << error.toStdString();
    EXPECT_FALSE(model.renameResource(ce::ResourceId{"orders"}, QStringLiteral("Users"), error));

    ce::Actor clash;
    clash.id = ce::ActorId{"USER"};
    EXPECT_FALSE(model.saveActor({}, clash, error));
    EXPECT_FALSE(model.saveEnvironment({}, QStringLiteral("LOCAL"), {}, error));
    EXPECT_FALSE(
        model.renameOperation(ce::OperationId{"orders.list"}, QStringLiteral("Each"), error));
    EXPECT_FALSE(model.createOperation(ce::ResourceId{"orders"}, QStringLiteral("LIST"), {}, error)
                     .has_value());

    EXPECT_EQ(model.project().resources.size(), 3U);
    EXPECT_EQ(model.project().actors.size(), 1U);
}

TEST(ProjectModelRename, names_the_engine_cannot_store_are_rejected) {
    QTemporaryDir dir;
    ProjectModel model;
    ASSERT_TRUE(load(dir, buildReferenceProject(), model));

    QString error;
    EXPECT_FALSE(model.createResource(QStringLiteral("c:d"), {}, error));
    EXPECT_FALSE(model.saveEnvironment({}, QStringLiteral("a:b"), {}, error));
}

TEST(ProjectModelRename, resource_rename_rewrites_references_and_for_each) {
    QTemporaryDir dir;
    ProjectModel model;
    ASSERT_TRUE(load(dir, buildReferenceProject(), model));

    QString error;
    ASSERT_TRUE(model.renameResource(ce::ResourceId{"users"}, QStringLiteral("people"), error))
        << error.toStdString();

    ProjectModel reloaded;
    reloaded.loadFromDirectory(dir.path());
    ASSERT_TRUE(reloaded.hasProject());
    const auto& list = opIn(reloaded, "orders.list");
    EXPECT_EQ(list.pathTemplate, "/orders/{{ people.id }}");
    EXPECT_EQ(list.headers.at("X-Note"), "users.id");
    EXPECT_EQ(list.queryParams.at("h"), "{{$.base64(people.id)}}");
    EXPECT_EQ(list.pollUntil->pathTemplate, "/status/{{people.id}}");
    EXPECT_EQ(opIn(reloaded, "orders.each").forEach->over.value, "people");
    EXPECT_EQ(opIn(reloaded, "audit.log").explicitDependencies.at(0).value, "people.create");
    EXPECT_EQ(reloaded.project().environments.at("local").at("who"), "{{people.id}}");
}

TEST(ProjectModelRename, rename_emits_operation_renamed_before_saved) {
    QTemporaryDir dir;
    ProjectModel model;
    ASSERT_TRUE(load(dir, buildReferenceProject(), model));
    std::vector<std::string> events;
    QObject receiver;
    QObject::connect(&model,
                     &ProjectModel::operationRenamed,
                     &receiver,
                     [&events](const QString& oldId, const QString& newId) {
                         events.push_back((oldId + QStringLiteral("->") + newId).toStdString());
                     });
    QObject::connect(
        &model, &ProjectModel::operationRemoved, &receiver, [&events](const QString& id) {
            events.push_back("removed " + id.toStdString());
        });
    QObject::connect(
        &model, &ProjectModel::saved, &receiver, [&events]() { events.emplace_back("saved"); });

    QString error;
    ASSERT_TRUE(model.renameResource(ce::ResourceId{"orders"}, QStringLiteral("sales"), error));
    ASSERT_TRUE(model.renameOperation(ce::OperationId{"sales.each"}, QStringLiteral("all"), error));
    ASSERT_TRUE(model.deleteOperation(ce::OperationId{"sales.all"}, error));

    const std::vector<std::string> expected{"orders.each->sales.each",
                                            "orders.list->sales.list",
                                            "saved",
                                            "sales.each->sales.all",
                                            "saved",
                                            "removed sales.all",
                                            "saved"};
    EXPECT_EQ(events, expected);
}

TEST(ProjectModelRename, actor_rename_rewrites_actor_references_and_poll_actor) {
    QTemporaryDir dir;
    ProjectModel model;
    ASSERT_TRUE(load(dir, buildReferenceProject(), model));

    QString error;
    ce::Actor renamed = model.project().actors.at(ce::ActorId{"user"});
    renamed.id = ce::ActorId{"admin"};
    ASSERT_TRUE(model.saveActor(QStringLiteral("user"), renamed, error)) << error.toStdString();

    ProjectModel reloaded;
    reloaded.loadFromDirectory(dir.path());
    ASSERT_TRUE(reloaded.hasProject());
    const auto& list = opIn(reloaded, "orders.list");
    EXPECT_EQ(list.actor.value, "admin");
    EXPECT_EQ(list.headers.at("X-Token"), "{{admin.token}}");
    EXPECT_EQ(list.pollUntil->actor->value, "admin");
    EXPECT_EQ(reloaded.project().actors.at(ce::ActorId{"admin"}).inject.headers.at("Authorization"),
              "Bearer {{admin.token}}");
}

TEST(ProjectModelRename, delete_resource_is_refused_while_referenced) {
    QTemporaryDir dir;
    ProjectModel model;
    ASSERT_TRUE(load(dir, buildReferenceProject(), model));
    const fs::path root{dir.path().toStdString()};

    QString error;
    EXPECT_FALSE(model.deleteResource(ce::ResourceId{"users"}, error));
    EXPECT_TRUE(error.contains(QStringLiteral("orders.list"))) << error.toStdString();
    EXPECT_TRUE(error.contains(QStringLiteral("orders.each"))) << error.toStdString();
    EXPECT_NE(model.findOperation(ce::OperationId{"users.create"}), nullptr);
    EXPECT_TRUE(fs::exists(root / "resources" / "users.yaml"));
}

TEST(ProjectModelRename, delete_resource_drops_depends_on_and_reports_the_operations) {
    QTemporaryDir dir;
    ProjectModel model;
    ce::Project project = buildReferenceProject();
    project.resources.erase(ce::ResourceId{"orders"});
    project.environments.at("local").erase("who");
    ASSERT_TRUE(load(dir, project, model));

    EXPECT_EQ(model.dependentsOf(QStringLiteral("resource"), QStringLiteral("users")),
              QStringList{QStringLiteral("audit.log")});
    QString error;
    QStringList dropped;
    ASSERT_TRUE(model.deleteResource(ce::ResourceId{"users"}, error, &dropped))
        << error.toStdString();
    EXPECT_EQ(dropped, QStringList{QStringLiteral("audit.log")});
    EXPECT_TRUE(opIn(model, "audit.log").explicitDependencies.empty());
}

TEST(ProjectModelRename, delete_operation_reports_depends_on_it_dropped) {
    QTemporaryDir dir;
    ProjectModel model;
    ce::Project project = buildReferenceProject();
    auto& create = project.resources.at(ce::ResourceId{"users"}).operations.at("create");
    create.extractions.clear();
    ASSERT_TRUE(load(dir, project, model));

    EXPECT_EQ(model.dependentsOf(QStringLiteral("operation"), QStringLiteral("users.create")),
              QStringList{QStringLiteral("audit.log")});
    QString error;
    QStringList dropped;
    ASSERT_TRUE(model.deleteOperation(ce::OperationId{"users.create"}, error, &dropped))
        << error.toStdString();
    EXPECT_EQ(dropped, QStringList{QStringLiteral("audit.log")});
}

TEST(ProjectModelRename, delete_operation_is_refused_while_it_alone_produces_a_used_variable) {
    QTemporaryDir dir;
    ProjectModel model;
    ASSERT_TRUE(load(dir, buildReferenceProject(), model));

    QString error;
    EXPECT_FALSE(model.deleteOperation(ce::OperationId{"users.create"}, error));
    EXPECT_TRUE(error.contains(QStringLiteral("orders.list"))) << error.toStdString();
    EXPECT_NE(model.findOperation(ce::OperationId{"users.create"}), nullptr);
}

TEST(ProjectModelRename, delete_actor_is_refused_while_templates_refer_to_it) {
    QTemporaryDir dir;
    ProjectModel model;
    ASSERT_TRUE(load(dir, buildReferenceProject(), model));

    QString error;
    EXPECT_FALSE(model.deleteActor(ce::ActorId{"user"}, error));
    EXPECT_TRUE(error.contains(QStringLiteral("orders.list"))) << error.toStdString();
    EXPECT_TRUE(model.project().actors.contains(ce::ActorId{"user"}));
}

TEST(ProjectModelRename, deleting_the_only_environment_is_refused) {
    QTemporaryDir dir;
    ProjectModel model;
    ASSERT_TRUE(load(dir, buildReferenceProject(), model));

    QString error;
    EXPECT_FALSE(model.deleteEnvironment(QStringLiteral("local"), error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_TRUE(model.project().environments.contains("local"));
}

// An actor edited and saved in the app keeps its `allowed_hosts` (FR-15.9): dropping it would
// silently lift the restriction.
TEST(ProjectModelRename, saving_an_actor_keeps_its_allowed_hosts) {
    QTemporaryDir dir;
    ProjectModel model;
    ASSERT_TRUE(load(dir, buildReferenceProject(), model));
    QString error;
    ce::Actor restricted = model.project().actors.at(ce::ActorId{"user"});
    restricted.allowedHosts = {"api.example.com", "auth.example.com:8443"};
    ASSERT_TRUE(model.saveActor(QStringLiteral("user"), restricted, error)) << error.toStdString();

    // A later edit starts from the loaded actor, as the actor editor does.
    ce::Actor edited = model.project().actors.at(ce::ActorId{"user"});
    edited.description = "edited";
    ASSERT_TRUE(model.saveActor(QStringLiteral("user"), edited, error)) << error.toStdString();

    ProjectModel reloaded;
    reloaded.loadFromDirectory(dir.path());
    ASSERT_TRUE(reloaded.hasProject());
    EXPECT_EQ(reloaded.project().actors.at(ce::ActorId{"user"}).allowedHosts,
              (std::vector<std::string>{"api.example.com", "auth.example.com:8443"}));
}

}  // namespace reqloom::desktop::tests
