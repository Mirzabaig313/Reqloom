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

namespace {

/// Two actors, two resources, three operations between them — enough for each
/// folder row's badge to count something different.
ce::Project makeCountedProject() {
    ce::Project project;
    project.name = "Counted";
    project.actors.emplace(ce::ActorId{"worker"}, ce::Actor{});
    project.actors.emplace(ce::ActorId{"employer"}, ce::Actor{});

    ce::Resource cart;
    cart.id = ce::ResourceId{"cart"};
    for (const auto* name : {"add", "remove"}) {
        ce::Operation op;
        op.id = ce::OperationId{std::string{"cart."} + name};
        op.method = ce::HttpMethod::Post;
        cart.operations.emplace(name, std::move(op));
    }
    project.resources.emplace(cart.id, std::move(cart));

    ce::Resource orders;
    orders.id = ce::ResourceId{"orders"};
    ce::Operation list;
    list.id = ce::OperationId{"orders.list"};
    list.method = ce::HttpMethod::Get;
    orders.operations.emplace("list", std::move(list));
    project.resources.emplace(orders.id, std::move(orders));

    return project;
}

}  // namespace

TEST(ProjectTreeModel, project_badge_counts_endpoints_not_its_two_group_folders) {
    ProjectTreeModel model;
    const auto counted = makeCountedProject();
    model.populate({entry("/ws/counted", "Counted", counted, true)});

    const QModelIndex project = model.index(0, 0, {});

    // Direct children are only Actors + Resources, so a raw child count would
    // read "2" for a project holding three endpoints.
    EXPECT_EQ(model.data(project, ProjectTreeModel::CountRole).toInt(), 3);
    EXPECT_EQ(model.data(project, ProjectTreeModel::CountLabelRole).toString(),
              QStringLiteral("3 endpoints"));
}

TEST(ProjectTreeModel, group_badges_name_the_unit_they_counted) {
    ProjectTreeModel model;
    const auto counted = makeCountedProject();
    model.populate({entry("/ws/counted", "Counted", counted, true)});

    const QModelIndex project = model.index(0, 0, {});
    const QModelIndex actors = model.index(0, 0, project);
    const QModelIndex resources = model.index(1, 0, project);

    EXPECT_EQ(model.data(actors, ProjectTreeModel::CountRole).toInt(), 2);
    EXPECT_EQ(model.data(actors, ProjectTreeModel::CountLabelRole).toString(),
              QStringLiteral("2 actors"));
    EXPECT_EQ(model.data(resources, ProjectTreeModel::CountRole).toInt(), 2);
    EXPECT_EQ(model.data(resources, ProjectTreeModel::CountLabelRole).toString(),
              QStringLiteral("2 resources"));
}

TEST(ProjectTreeModel, resource_badge_counts_its_own_endpoints) {
    ProjectTreeModel model;
    const auto counted = makeCountedProject();
    model.populate({entry("/ws/counted", "Counted", counted, true)});

    const QModelIndex resources = model.index(1, 0, model.index(0, 0, {}));
    const QModelIndex cart = model.index(0, 0, resources);

    EXPECT_EQ(model.data(cart, ProjectTreeModel::CountRole).toInt(), 2);
    EXPECT_EQ(model.data(cart, ProjectTreeModel::CountLabelRole).toString(),
              QStringLiteral("2 endpoints"));
}

TEST(ProjectTreeModel, leaf_rows_carry_no_badge) {
    ProjectTreeModel model;
    const auto counted = makeCountedProject();
    model.populate({entry("/ws/counted", "Counted", counted, true)});

    const QModelIndex op = operationIndex(model, 0);

    EXPECT_EQ(model.data(op, ProjectTreeModel::CountRole).toInt(), 0);
    EXPECT_TRUE(model.data(op, ProjectTreeModel::CountLabelRole).toString().isEmpty());
}

TEST(ProjectTreeModel, singular_badge_label_drops_the_plural) {
    ProjectTreeModel model;
    const auto single = makeProject("Single");  // one resource, one operation
    model.populate({entry("/ws/single", "Single", single, true)});

    const QModelIndex project = model.index(0, 0, {});

    EXPECT_EQ(model.data(project, ProjectTreeModel::CountLabelRole).toString(),
              QStringLiteral("1 endpoint"));
}

namespace {

// Navigate project node → Actors group → actor row. Actors are sorted by id,
// so row 0 of makeCountedProject() is "employer" and row 1 is "worker".
QModelIndex actorIndex(const ProjectTreeModel& model, int projectRow, int actorRow) {
    const QModelIndex actors = model.index(0, 0, model.index(projectRow, 0, {}));
    return model.index(actorRow, 0, actors);
}

}  // namespace

TEST(ProjectTreeModel, actor_rows_report_no_session_until_one_is_pushed) {
    ProjectTreeModel model;
    const auto counted = makeCountedProject();
    model.populate({entry("/ws/counted", "Counted", counted, true)});

    const QModelIndex actor = actorIndex(model, 0, 0);

    EXPECT_EQ(model.data(actor, ProjectTreeModel::SessionStateRole).toString(),
              QStringLiteral("none"));
    EXPECT_EQ(model.data(actor, ProjectTreeModel::SessionSecondsRole).toInt(), 0);
}

TEST(ProjectTreeModel, actor_row_surfaces_its_pushed_session_state_and_ttl) {
    ProjectTreeModel model;
    const auto counted = makeCountedProject();
    model.populate({entry("/ws/counted", "Counted", counted, true)});

    QMap<QString, ProjectTreeModel::ActorSessionRow> sessions;
    sessions.insert(
        ProjectTreeModel::sessionKey(QStringLiteral("/ws/counted"), QStringLiteral("worker")),
        ProjectTreeModel::ActorSessionRow{QStringLiteral("live"), 95});
    model.setActorSessions(sessions);

    const QModelIndex worker = actorIndex(model, 0, 1);
    EXPECT_EQ(model.data(worker, ProjectTreeModel::SessionStateRole).toString(),
              QStringLiteral("live"));
    EXPECT_EQ(model.data(worker, ProjectTreeModel::SessionSecondsRole).toInt(), 95);

    // The actor nobody signed in as stays untouched.
    const QModelIndex employer = actorIndex(model, 0, 0);
    EXPECT_EQ(model.data(employer, ProjectTreeModel::SessionStateRole).toString(),
              QStringLiteral("none"));
}

TEST(ProjectTreeModel, sessions_do_not_leak_between_projects_sharing_an_actor_name) {
    ProjectTreeModel model;
    const auto alpha = makeCountedProject();
    const auto beta = makeCountedProject();
    model.populate(
        {entry("/ws/alpha", "Alpha", alpha, true), entry("/ws/beta", "Beta", beta, false)});

    QMap<QString, ProjectTreeModel::ActorSessionRow> sessions;
    sessions.insert(
        ProjectTreeModel::sessionKey(QStringLiteral("/ws/alpha"), QStringLiteral("worker")),
        ProjectTreeModel::ActorSessionRow{QStringLiteral("live"), 300});
    model.setActorSessions(sessions);

    EXPECT_EQ(model.data(actorIndex(model, 0, 1), ProjectTreeModel::SessionStateRole).toString(),
              QStringLiteral("live"));
    // Beta has an actor named "worker" too, but its own run context has no session.
    EXPECT_EQ(model.data(actorIndex(model, 1, 1), ProjectTreeModel::SessionStateRole).toString(),
              QStringLiteral("none"));
}

TEST(ProjectTreeModel, non_actor_rows_carry_no_session_state) {
    ProjectTreeModel model;
    const auto counted = makeCountedProject();
    model.populate({entry("/ws/counted", "Counted", counted, true)});

    const QModelIndex op = operationIndex(model, 0);

    EXPECT_TRUE(model.data(op, ProjectTreeModel::SessionStateRole).toString().isEmpty());
    EXPECT_EQ(model.data(op, ProjectTreeModel::SessionSecondsRole).toInt(), 0);
}

TEST(ProjectTreeModel, pushing_sessions_repaints_actor_rows_without_resetting_the_tree) {
    ProjectTreeModel model;
    const auto counted = makeCountedProject();
    model.populate({entry("/ws/counted", "Counted", counted, true)});

    int resets = 0;
    int changes = 0;
    QObject::connect(&model, &QAbstractItemModel::modelAboutToBeReset, [&resets] { ++resets; });
    QObject::connect(&model, &QAbstractItemModel::dataChanged, [&changes] { ++changes; });

    QMap<QString, ProjectTreeModel::ActorSessionRow> sessions;
    sessions.insert(
        ProjectTreeModel::sessionKey(QStringLiteral("/ws/counted"), QStringLiteral("worker")),
        ProjectTreeModel::ActorSessionRow{QStringLiteral("live"), 42});
    model.setActorSessions(sessions);

    // Expansion state and selection must survive a session change mid-session.
    EXPECT_EQ(resets, 0);
    EXPECT_EQ(changes, 1);

    // Re-pushing the same snapshot is a no-op, so a poll can't churn the view.
    model.setActorSessions(sessions);
    EXPECT_EQ(changes, 1);
}

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

TEST(ProjectTreeModel, combined_populate_shows_examples_for_all_open_projects) {
    // Phase 4: the tree carries every open collection's examples at once (not
    // just the active project's), applied in a single reset.
    ProjectTreeModel model;
    const auto pa = makeProject("Alpha");
    const auto pb = makeProject("Beta");

    QMap<QString, QList<ProjectTreeModel::ExampleRow>> examples;
    examples.insert(
        ProjectTreeModel::exampleKey(QStringLiteral("/ws/alpha"), QStringLiteral("cart.add")),
        {ProjectTreeModel::ExampleRow{QStringLiteral("a-ex"), 200}});
    examples.insert(
        ProjectTreeModel::exampleKey(QStringLiteral("/ws/beta"), QStringLiteral("cart.add")),
        {ProjectTreeModel::ExampleRow{QStringLiteral("b-ex"), 201}});

    model.populate({entry("/ws/alpha", "Alpha", pa, true), entry("/ws/beta", "Beta", pb, false)},
                   examples);

    // Both collections' operations carry their own example rows.
    EXPECT_EQ(model.rowCount(operationIndex(model, 0)), 1);
    EXPECT_EQ(model.rowCount(operationIndex(model, 1)), 1);
}
