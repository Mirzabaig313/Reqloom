// ProjectTreeModel aggregation: multiple open collections become sibling
// Project nodes, every row carries its owning project root, and saved-example
// rows are scoped per-project. Multi-Project Workspace Plan, Phase 3.
#include "ProjectTreeModel.h"

#include <reqloom/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <QtCore/QList>
#include <QtCore/QMap>
#include <QtCore/QModelIndex>

namespace ce = reqloom::engine;
using reqloom::desktop::qml::ProjectTreeModel;

namespace {

ce::Project makeProject(const std::string& name) {
    ce::Project project;
    project.name = name;
    ce::Resource resource;
    resource.id = ce::ResourceId{"cart"};
    ce::Operation op;
    op.id = ce::OperationId{"cart.add"};
    op.method = ce::HttpMethod::Post;
    op.pathTemplate = "/cart/items";
    resource.operations.emplace("add", std::move(op));
    project.resources.emplace(resource.id, std::move(resource));
    return project;
}

ProjectTreeModel::ProjectEntry entry(const QString& root,
                                     const QString& name,
                                     const ce::Project& project,
                                     bool active) {
    ProjectTreeModel::ProjectEntry e;
    e.root = root;
    e.name = name;
    e.project = std::make_shared<const ce::Project>(project);
    e.active = active;
    return e;
}

// Navigate project node → Resources group → resource → operation.
QModelIndex operationIndex(const ProjectTreeModel& model, int projectRow) {
    const QModelIndex project = model.index(projectRow, 0, {});
    const QModelIndex resources = model.index(1, 0, project);  // 0=Actors, 1=Resources
    const QModelIndex resource = model.index(0, 0, resources);
    return model.index(0, 0, resource);
}

}  // namespace

TEST(ProjectTreeModel, aggregates_open_collections_as_sibling_project_nodes) {
    ProjectTreeModel model;
    const auto pa = makeProject("Alpha");
    const auto pb = makeProject("Beta");
    model.populate({entry("/ws/alpha", "Alpha", pa, true), entry("/ws/beta", "Beta", pb, false)});

    ASSERT_EQ(model.rowCount({}), 2);

    const QModelIndex p0 = model.index(0, 0, {});
    EXPECT_EQ(model.data(p0, ProjectTreeModel::KindRole).toString(), QStringLiteral("project"));
    EXPECT_EQ(model.data(p0, ProjectTreeModel::NameRole).toString(), QStringLiteral("Alpha"));
    EXPECT_EQ(model.data(p0, ProjectTreeModel::ProjectRootRole).toString(),
              QStringLiteral("/ws/alpha"));
    EXPECT_TRUE(model.data(p0, ProjectTreeModel::ActiveRole).toBool());

    const QModelIndex p1 = model.index(1, 0, {});
    EXPECT_FALSE(model.data(p1, ProjectTreeModel::ActiveRole).toBool());
}

TEST(ProjectTreeModel, every_operation_row_carries_its_owning_project_root) {
    ProjectTreeModel model;
    const auto pa = makeProject("Alpha");
    const auto pb = makeProject("Beta");
    model.populate({entry("/ws/alpha", "Alpha", pa, true), entry("/ws/beta", "Beta", pb, false)});

    const QModelIndex opA = operationIndex(model, 0);
    ASSERT_TRUE(opA.isValid());
    EXPECT_EQ(model.data(opA, ProjectTreeModel::OperationIdRole).toString(),
              QStringLiteral("cart.add"));
    EXPECT_EQ(model.data(opA, ProjectTreeModel::ProjectRootRole).toString(),
              QStringLiteral("/ws/alpha"));

    // The identically-named op in Beta resolves to Beta's root — no collision.
    const QModelIndex opB = operationIndex(model, 1);
    ASSERT_TRUE(opB.isValid());
    EXPECT_EQ(model.data(opB, ProjectTreeModel::ProjectRootRole).toString(),
              QStringLiteral("/ws/beta"));
}

TEST(ProjectTreeModel, saved_examples_are_scoped_per_project) {
    ProjectTreeModel model;
    const auto pa = makeProject("Alpha");
    const auto pb = makeProject("Beta");
    model.populate({entry("/ws/alpha", "Alpha", pa, true), entry("/ws/beta", "Beta", pb, false)});

    // Attach an example to Alpha's cart.add only.
    QMap<QString, QList<ProjectTreeModel::ExampleRow>> examples;
    examples.insert(
        ProjectTreeModel::exampleKey(QStringLiteral("/ws/alpha"), QStringLiteral("cart.add")),
        {ProjectTreeModel::ExampleRow{QStringLiteral("saved-1"), 200}});
    model.setSavedExamples(examples);

    // Alpha's op gets the example child; Beta's identically-named op does not.
    EXPECT_EQ(model.rowCount(operationIndex(model, 0)), 1);
    EXPECT_EQ(model.rowCount(operationIndex(model, 1)), 0);
}
