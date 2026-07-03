// WorkspaceModel container semantics: it always holds an active project, can
// grow, and switches the active index with bounds-checking. Multi-Project
// Workspace Plan, Phase 1.
#include "application/WorkspaceModel.h"

#include "application/ProjectModel.h"

#include <gtest/gtest.h>

#include <QtCore/QObject>
#include <QtCore/QTemporaryDir>

namespace reqloom::desktop::tests {

TEST(WorkspaceModel, starts_with_one_unloaded_active_project) {
    WorkspaceModel ws;
    EXPECT_EQ(ws.count(), 1);
    EXPECT_EQ(ws.activeIndex(), 0);
    ASSERT_NE(ws.active(), nullptr);
    // Sentinel project: present so hasProject() guards stay valid, but nothing
    // is loaded until the app opens/imports something.
    EXPECT_FALSE(ws.active()->hasProject());
}

TEST(WorkspaceModel, add_project_appends_without_changing_active) {
    WorkspaceModel ws;
    ProjectModel* added = ws.addProject();
    ASSERT_NE(added, nullptr);
    EXPECT_EQ(ws.count(), 2);
    EXPECT_EQ(ws.activeIndex(), 0);
    EXPECT_EQ(ws.at(1), added);
    EXPECT_EQ(ws.active(), ws.at(0));
}

TEST(WorkspaceModel, set_active_index_moves_active_and_signals) {
    WorkspaceModel ws;
    ws.addProject();
    int changes = 0;
    QObject::connect(&ws, &WorkspaceModel::activeChanged, [&changes]() { ++changes; });

    ws.setActiveIndex(1);
    EXPECT_EQ(ws.activeIndex(), 1);
    EXPECT_EQ(ws.active(), ws.at(1));
    EXPECT_EQ(changes, 1);
}

TEST(WorkspaceModel, set_active_index_is_bounds_checked_and_no_op_safe) {
    WorkspaceModel ws;
    ws.addProject();
    ws.setActiveIndex(1);
    int changes = 0;
    QObject::connect(&ws, &WorkspaceModel::activeChanged, [&changes]() { ++changes; });

    ws.setActiveIndex(5);   // out of range → ignored
    ws.setActiveIndex(-1);  // out of range → ignored
    ws.setActiveIndex(1);   // no-op (already active) → no signal
    EXPECT_EQ(ws.activeIndex(), 1);
    EXPECT_EQ(changes, 0);
    EXPECT_EQ(ws.at(9), nullptr);
}

TEST(WorkspaceModel, active_project_is_usable_for_scaffolding) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    WorkspaceModel ws;
    QString error;
    ASSERT_TRUE(ws.active()->createProject(dir.path(), "WS", error)) << error.toStdString();
    EXPECT_TRUE(ws.active()->hasProject());
    EXPECT_EQ(ws.active()->name().toStdString(), "WS");
}

TEST(WorkspaceModel, index_of_root_finds_open_project_or_reports_absent) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    WorkspaceModel ws;
    QString error;
    ASSERT_TRUE(ws.active()->createProject(dir.path(), "WS", error)) << error.toStdString();

    EXPECT_EQ(ws.indexOfRoot(ws.active()->rootPath()), 0);
    EXPECT_EQ(ws.indexOfRoot(QStringLiteral("/no/such/project")), -1);
}

TEST(WorkspaceModel, remove_middle_keeps_active_project_identity) {
    WorkspaceModel ws;                      // index 0
    ws.addProject();                        // index 1
    ProjectModel* third = ws.addProject();  // index 2
    ws.setActiveIndex(2);
    ASSERT_EQ(ws.active(), third);

    ws.removeProject(1);  // active (2) shifts to 1, same object
    EXPECT_EQ(ws.count(), 2);
    EXPECT_EQ(ws.activeIndex(), 1);
    EXPECT_EQ(ws.active(), third);
}

TEST(WorkspaceModel, remove_active_clamps_to_neighbour_and_signals) {
    WorkspaceModel ws;
    ws.addProject();
    ws.addProject();  // 3 projects, indices 0,1,2
    ws.setActiveIndex(2);
    int changes = 0;
    QObject::connect(&ws, &WorkspaceModel::activeChanged, [&changes]() { ++changes; });

    ws.removeProject(2);  // active removed → clamp to last surviving (1)
    EXPECT_EQ(ws.count(), 2);
    EXPECT_EQ(ws.activeIndex(), 1);
    EXPECT_EQ(changes, 1);
}

TEST(WorkspaceModel, removing_last_project_leaves_a_fresh_sentinel) {
    WorkspaceModel ws;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QString error;
    ASSERT_TRUE(ws.active()->createProject(dir.path(), "Solo", error)) << error.toStdString();

    ws.removeProject(0);
    EXPECT_EQ(ws.count(), 1);
    EXPECT_EQ(ws.activeIndex(), 0);
    ASSERT_NE(ws.active(), nullptr);
    EXPECT_FALSE(ws.active()->hasProject());  // fresh, unloaded
}

}  // namespace reqloom::desktop::tests
