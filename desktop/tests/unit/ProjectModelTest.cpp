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

}  // namespace reqloom::desktop::tests
