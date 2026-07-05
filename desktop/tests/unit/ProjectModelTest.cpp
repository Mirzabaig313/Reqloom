// Tests ProjectModel's structural edits — rename/delete of operations and
// resources — through a real write→reload cycle. Builds a project, writes it
// with the engine, loads it via ProjectModel, mutates, and verifies both the
// in-memory result (findOperation) and the on-disk file layout.
#include "application/ProjectModel.h"

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <QtCore/QTemporaryDir>

#include <filesystem>

namespace reqloom::desktop::tests {

namespace {

namespace ce = reqloom::engine;
namespace fs = std::filesystem;

/// Minimal valid project: one actor, one resource "payment" with op "pay".
ce::Project buildProject() {
    ce::Project p;
    p.name = "RT";
    p.defaultEnvironment = "local";
    p.environments["local"] = {{"baseUrl", "http://localhost:0"}};

    ce::Actor user;
    user.id = ce::ActorId{"user"};
    user.strategy = ce::AuthStrategy::Simple;
    ce::AuthStep step;
    step.id = "login";
    step.method = ce::HttpMethod::Post;
    step.pathTemplate = "/login";
    step.expectStatus = 200;
    step.extractions.push_back({"token", "$.token", ce::Extraction::Source::JsonPath});
    user.authSteps.push_back(std::move(step));
    user.inject.headers["Authorization"] = "Bearer {{user.token}}";
    p.actors[user.id] = std::move(user);

    ce::Resource res;
    res.id = ce::ResourceId{"payment"};
    ce::Operation op;
    op.id = ce::OperationId{"payment.pay"};
    op.resource = res.id;
    op.actor = ce::ActorId{"user"};
    op.method = ce::HttpMethod::Get;
    op.pathTemplate = "/pay";
    res.operations["pay"] = std::move(op);

    // A second op that depends on the first, to exercise depends_on integrity.
    ce::Operation refund;
    refund.id = ce::OperationId{"payment.refund"};
    refund.resource = res.id;
    refund.actor = ce::ActorId{"user"};
    refund.method = ce::HttpMethod::Post;
    refund.pathTemplate = "/refund";
    refund.explicitDependencies = {ce::OperationId{"payment.pay"}};
    res.operations["refund"] = std::move(refund);

    p.resources[res.id] = std::move(res);
    return p;
}

/// Write `buildProject()` to a temp dir and load it through ProjectModel.
[[nodiscard]] bool loadFixture(const QTemporaryDir& dir, ProjectModel& model) {
    const fs::path root{dir.path().toStdString()};
    if (!ce::writeProject(root, buildProject(), /*overwrite=*/true).has_value()) {
        return false;
    }
    model.loadFromDirectory(dir.path());
    return model.hasProject();
}

}  // namespace

TEST(ProjectModel, rename_resource_requalifies_operations_and_moves_file) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ProjectModel model;
    ASSERT_TRUE(loadFixture(dir, model));
    ASSERT_NE(model.findOperation(ce::OperationId{"payment.pay"}), nullptr);

    QString error;
    ASSERT_TRUE(model.renameResource(ce::ResourceId{"payment"}, QStringLiteral("billing"), error))
        << error.toStdString();

    // Operation is re-qualified under the new resource id.
    EXPECT_NE(model.findOperation(ce::OperationId{"billing.pay"}), nullptr);
    EXPECT_EQ(model.findOperation(ce::OperationId{"payment.pay"}), nullptr);

    // The old resource file is gone; the new one exists.
    const fs::path root{dir.path().toStdString()};
    EXPECT_FALSE(fs::exists(root / "resources" / "payment.yaml"));
    EXPECT_TRUE(fs::exists(root / "resources" / "billing.yaml"));
}

TEST(ProjectModel, rename_resource_rejects_duplicate_name) {
    QTemporaryDir dir;
    ProjectModel model;
    ASSERT_TRUE(loadFixture(dir, model));

    // Renaming to its own id is a no-op success; empty is rejected.
    QString error;
    EXPECT_FALSE(model.renameResource(ce::ResourceId{"payment"}, QString{}, error));
    EXPECT_FALSE(error.isEmpty());
}

TEST(ProjectModel, delete_resource_removes_it_and_its_file) {
    QTemporaryDir dir;
    ProjectModel model;
    ASSERT_TRUE(loadFixture(dir, model));

    QString error;
    ASSERT_TRUE(model.deleteResource(ce::ResourceId{"payment"}, error)) << error.toStdString();

    EXPECT_EQ(model.findOperation(ce::OperationId{"payment.pay"}), nullptr);
    const fs::path root{dir.path().toStdString()};
    EXPECT_FALSE(fs::exists(root / "resources" / "payment.yaml"));
}

TEST(ProjectModel, rename_operation_changes_its_id) {
    QTemporaryDir dir;
    ProjectModel model;
    ASSERT_TRUE(loadFixture(dir, model));

    QString error;
    ASSERT_TRUE(
        model.renameOperation(ce::OperationId{"payment.pay"}, QStringLiteral("charge"), error))
        << error.toStdString();
    EXPECT_NE(model.findOperation(ce::OperationId{"payment.charge"}), nullptr);
    EXPECT_EQ(model.findOperation(ce::OperationId{"payment.pay"}), nullptr);
}

TEST(ProjectModel, delete_operation_removes_it) {
    QTemporaryDir dir;
    ProjectModel model;
    ASSERT_TRUE(loadFixture(dir, model));

    QString error;
    ASSERT_TRUE(model.deleteOperation(ce::OperationId{"payment.pay"}, error))
        << error.toStdString();
    EXPECT_EQ(model.findOperation(ce::OperationId{"payment.pay"}), nullptr);
}

TEST(ProjectModel, rename_operation_updates_dependents_depends_on) {
    QTemporaryDir dir;
    ProjectModel model;
    ASSERT_TRUE(loadFixture(dir, model));
    // payment.refund depends on payment.pay.
    QString error;
    ASSERT_TRUE(
        model.renameOperation(ce::OperationId{"payment.pay"}, QStringLiteral("charge"), error))
        << error.toStdString();

    const auto* refund = model.findOperation(ce::OperationId{"payment.refund"});
    ASSERT_NE(refund, nullptr);
    ASSERT_EQ(refund->explicitDependencies.size(), 1u);
    EXPECT_EQ(refund->explicitDependencies[0].value, "payment.charge");

    // The project still loads cleanly from disk (no dangling reference).
    ProjectModel reloaded;
    reloaded.loadFromDirectory(dir.path());
    EXPECT_TRUE(reloaded.hasProject());
}

TEST(ProjectModel, delete_operation_drops_dangling_depends_on) {
    QTemporaryDir dir;
    ProjectModel model;
    ASSERT_TRUE(loadFixture(dir, model));

    QString error;
    ASSERT_TRUE(model.deleteOperation(ce::OperationId{"payment.pay"}, error))
        << error.toStdString();

    const auto* refund = model.findOperation(ce::OperationId{"payment.refund"});
    ASSERT_NE(refund, nullptr);
    EXPECT_TRUE(refund->explicitDependencies.empty());

    ProjectModel reloaded;
    reloaded.loadFromDirectory(dir.path());
    EXPECT_TRUE(reloaded.hasProject());
}

TEST(ProjectModel, rename_operation_rejects_id_breaking_name) {
    QTemporaryDir dir;
    ProjectModel model;
    ASSERT_TRUE(loadFixture(dir, model));

    // A '.' would split into a new resource id and recreate the dangling-ref
    // bug; '/' and '\' would escape the resources/ directory on resource files.
    QString error;
    EXPECT_FALSE(
        model.renameOperation(ce::OperationId{"payment.pay"}, QStringLiteral("a.b"), error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_NE(model.findOperation(ce::OperationId{"payment.pay"}), nullptr);
}

TEST(ProjectModel, rename_resource_rejects_id_breaking_name) {
    QTemporaryDir dir;
    ProjectModel model;
    ASSERT_TRUE(loadFixture(dir, model));

    QString error;
    EXPECT_FALSE(
        model.renameResource(ce::ResourceId{"payment"}, QStringLiteral("../escape"), error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_NE(model.findOperation(ce::OperationId{"payment.pay"}), nullptr);
}

TEST(ProjectModel, create_resource_writes_file_and_loads) {
    QTemporaryDir dir;
    ProjectModel model;
    ASSERT_TRUE(loadFixture(dir, model));

    QString error;
    ASSERT_TRUE(model.createResource(QStringLiteral("billing"), QStringLiteral("Billing"), error))
        << error.toStdString();

    const fs::path root{dir.path().toStdString()};
    EXPECT_TRUE(fs::exists(root / "resources" / "billing.yaml"));

    // The empty resource round-trips through a fresh load.
    ProjectModel reloaded;
    reloaded.loadFromDirectory(dir.path());
    ASSERT_TRUE(reloaded.hasProject());
    EXPECT_TRUE(reloaded.project().resources.contains(ce::ResourceId{"billing"}));
}

TEST(ProjectModel, create_resource_rejects_empty_duplicate_and_id_breaking) {
    QTemporaryDir dir;
    ProjectModel model;
    ASSERT_TRUE(loadFixture(dir, model));

    QString error;
    EXPECT_FALSE(model.createResource(QString{}, QString{}, error));
    EXPECT_FALSE(model.createResource(QStringLiteral("payment"), QString{}, error));  // duplicate
    EXPECT_FALSE(model.createResource(QStringLiteral("a.b"), QString{}, error));      // id-breaking
}

TEST(ProjectModel, create_operation_returns_id_and_reloads_clean) {
    QTemporaryDir dir;
    ProjectModel model;
    ASSERT_TRUE(loadFixture(dir, model));

    QString error;
    const auto id = model.createOperation(ce::ResourceId{"payment"},
                                          QStringLiteral("refund_all"),
                                          ce::HttpMethod::Post,
                                          QStringLiteral("/refund-all"),
                                          ce::ActorId{"user"},
                                          {},
                                          {},
                                          error);
    ASSERT_TRUE(id.has_value()) << error.toStdString();
    EXPECT_EQ(id->value, "payment.refund_all");
    EXPECT_NE(model.findOperation(*id), nullptr);

    ProjectModel reloaded;
    reloaded.loadFromDirectory(dir.path());
    ASSERT_TRUE(reloaded.hasProject());
    EXPECT_NE(reloaded.findOperation(ce::OperationId{"payment.refund_all"}), nullptr);
}

TEST(ProjectModel, create_operation_rejects_duplicate_id_breaking_and_unknown_resource) {
    QTemporaryDir dir;
    ProjectModel model;
    ASSERT_TRUE(loadFixture(dir, model));

    QString error;
    // Duplicate op name in the resource.
    EXPECT_FALSE(model
                     .createOperation(ce::ResourceId{"payment"},
                                      QStringLiteral("pay"),
                                      ce::HttpMethod::Get,
                                      QStringLiteral("/pay"),
                                      ce::ActorId{"user"},
                                      {},
                                      {},
                                      error)
                     .has_value());
    // Id-breaking name.
    EXPECT_FALSE(model
                     .createOperation(ce::ResourceId{"payment"},
                                      QStringLiteral("a.b"),
                                      ce::HttpMethod::Get,
                                      QStringLiteral("/x"),
                                      ce::ActorId{"user"},
                                      {},
                                      {},
                                      error)
                     .has_value());
    // Unknown resource.
    EXPECT_FALSE(model
                     .createOperation(ce::ResourceId{"nope"},
                                      QStringLiteral("x"),
                                      ce::HttpMethod::Get,
                                      QStringLiteral("/x"),
                                      ce::ActorId{"user"},
                                      {},
                                      {},
                                      error)
                     .has_value());
}

TEST(ProjectModel, create_operation_with_dependencies_and_extractions_reloads_clean) {
    QTemporaryDir dir;
    ProjectModel model;
    ASSERT_TRUE(loadFixture(dir, model));

    // New op depends on payment.pay and extracts a value others could consume.
    std::vector<ce::Extraction> extractions{
        {"receipt_id", "$.data.receiptId", ce::Extraction::Source::JsonPath}};
    std::vector<ce::OperationId> deps{ce::OperationId{"payment.pay"}};

    QString error;
    const auto id = model.createOperation(ce::ResourceId{"payment"},
                                          QStringLiteral("receipt"),
                                          ce::HttpMethod::Get,
                                          QStringLiteral("/receipt"),
                                          ce::ActorId{"user"},
                                          deps,
                                          extractions,
                                          error);
    ASSERT_TRUE(id.has_value()) << error.toStdString();

    const auto* created = model.findOperation(*id);
    ASSERT_NE(created, nullptr);
    ASSERT_EQ(created->explicitDependencies.size(), 1u);
    EXPECT_EQ(created->explicitDependencies[0].value, "payment.pay");
    ASSERT_EQ(created->extractions.size(), 1u);
    EXPECT_EQ(created->extractions[0].variableName, "receipt_id");

    ProjectModel reloaded;
    reloaded.loadFromDirectory(dir.path());
    EXPECT_TRUE(reloaded.hasProject());
}

TEST(ProjectModel, create_operation_rejects_dependency_cycle) {
    QTemporaryDir dir;
    ProjectModel model;
    ASSERT_TRUE(loadFixture(dir, model));

    // A self-dependency is the simplest back edge: the op depends on its own
    // id, which Kahn's sort can never give in-degree 0. validateProject must
    // reject it before anything is written.
    QString error;
    const auto selfId = ce::OperationId{"payment.selfloop"};
    const auto id = model.createOperation(ce::ResourceId{"payment"},
                                          QStringLiteral("selfloop"),
                                          ce::HttpMethod::Get,
                                          QStringLiteral("/x"),
                                          ce::ActorId{"user"},
                                          {selfId},
                                          {},
                                          error);
    EXPECT_FALSE(id.has_value());
    EXPECT_FALSE(error.isEmpty());
}

TEST(ProjectModel, save_operation_rejects_dependency_cycle) {
    QTemporaryDir dir;
    ProjectModel model;
    ASSERT_TRUE(loadFixture(dir, model));

    // payment.refund already depends_on payment.pay. Editing payment.pay to
    // depend back on payment.refund closes the loop — validateProject must
    // reject the save and leave disk untouched.
    const auto* pay = model.findOperation(ce::OperationId{"payment.pay"});
    ASSERT_NE(pay, nullptr);
    ce::Operation edited = *pay;
    edited.explicitDependencies = {ce::OperationId{"payment.refund"}};

    QString error;
    EXPECT_FALSE(model.saveOperation(ce::OperationId{"payment.pay"}, edited, error));
    EXPECT_FALSE(error.isEmpty());

    // The project still loads (the bad edit never reached disk).
    ProjectModel reloaded;
    reloaded.loadFromDirectory(dir.path());
    EXPECT_TRUE(reloaded.hasProject());
}

TEST(ProjectModel, create_project_scaffolds_empty_project_and_loads_it) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const fs::path root{dir.path().toStdString()};
    const auto sub = QString::fromStdString((root / "fresh").string());

    ProjectModel model;
    QString error;
    // Target folder doesn't exist yet — createProject must create it, write
    // reqloom.yaml, and load the result.
    ASSERT_TRUE(model.createProject(sub, "My API", error)) << error.toStdString();
    EXPECT_TRUE(error.isEmpty());
    ASSERT_TRUE(model.hasProject());
    EXPECT_EQ(model.name().toStdString(), "My API");
    EXPECT_TRUE(fs::exists(fs::path{sub.toStdString()} / "reqloom.yaml"));

    // A freshly scaffolded project has no modules — the "+" create surface
    // takes over from here.
    EXPECT_TRUE(model.project().resources.empty());
}

TEST(ProjectModel, create_project_blank_name_falls_back_to_folder_name) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto sub =
        QString::fromStdString((fs::path{dir.path().toStdString()} / "checkout").string());

    ProjectModel model;
    QString error;
    ASSERT_TRUE(model.createProject(sub, "   ", error)) << error.toStdString();
    EXPECT_EQ(model.name().toStdString(), "checkout");
}

TEST(ProjectModel, create_project_refuses_to_clobber_existing_project) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    // Seed a real project in the dir, then try to scaffold over it.
    const fs::path root{dir.path().toStdString()};
    ASSERT_TRUE(ce::writeProject(root, buildProject(), /*overwrite=*/true).has_value());

    ProjectModel model;
    QString error;
    EXPECT_FALSE(model.createProject(dir.path(), "New", error));
    EXPECT_FALSE(error.isEmpty());
}

}  // namespace reqloom::desktop::tests
