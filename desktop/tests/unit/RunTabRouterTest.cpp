// RunTabRouter — a run's events stay with the tab that started it while the person switches tabs.
#include <gtest/gtest.h>

#include "appcontroller/RunTabRouter.h"
#include "models/TimelineModel.h"

#include <QtCore/QString>

namespace reqloom::desktop::qml::tests {

namespace {

const RunTabRouter::TabKey kOrders{0, QStringLiteral("orders.create")};
const RunTabRouter::TabKey kUsers{0, QStringLiteral("users.get")};

[[nodiscard]] QString roleAt(const TimelineModel& model, int row, int role) {
    return model.data(model.index(row, 0), role).toString();
}

/// An earlier, finished run of the users tab: header, one step, summary.
[[nodiscard]] TimelineModel::Snapshot finishedUsersRun() {
    TimelineModel model;
    model.onRunStarted(QStringLiteral("users.get"), 1, QStringLiteral("dev"));
    model.onStepStarted(0, QStringLiteral("users.get"), 1);
    model.onRunEnded(QStringLiteral("Succeeded"));
    return model.takeSnapshot();
}

/// Start an orders run in the visible timeline, then switch to the users tab the way AppController
/// does: park the visible timeline in the orders tab and show the users tab's.
[[nodiscard]] TimelineModel::Snapshot startOrdersRunThenSwitch(RunTabRouter& router,
                                                               TimelineModel& visible) {
    router.begin(kOrders);
    TimelineModel* live = router.timeline(visible, /*shown=*/true, nullptr);
    live->onRunStarted(QStringLiteral("orders.create"), 2, QStringLiteral("dev"));
    live->onStepStarted(0, QStringLiteral("auth.login"), 1);
    TimelineModel::Snapshot parkedOrders = visible.takeSnapshot();
    visible.restoreSnapshot(finishedUsersRun());
    return parkedOrders;
}

}  // namespace

TEST(RunTabRouter, events_for_a_hidden_origin_tab_leave_the_visible_timeline_untouched) {
    RunTabRouter router;
    TimelineModel visible;
    const TimelineModel::Snapshot parkedOrders = startOrdersRunThenSwitch(router, visible);

    TimelineModel* target = router.timeline(visible, /*shown=*/false, &parkedOrders);
    target->onStepStarted(1, QStringLiteral("orders.create"), 1);
    target->onRunEnded(QStringLiteral("Succeeded"));

    EXPECT_NE(target, &visible);
    ASSERT_EQ(visible.rowCount(), 3);
    EXPECT_EQ(roleAt(visible, 1, TimelineModel::OpRole), QStringLiteral("users.get"));
    EXPECT_EQ(roleAt(visible, 0, TimelineModel::StatusLabelRole), QStringLiteral("done"));
}

TEST(RunTabRouter, a_run_that_ends_while_its_tab_is_hidden_hands_back_a_settled_timeline) {
    RunTabRouter router;
    TimelineModel visible;
    const TimelineModel::Snapshot parkedOrders = startOrdersRunThenSwitch(router, visible);
    TimelineModel* target = router.timeline(visible, /*shown=*/false, &parkedOrders);
    target->onStepStarted(1, QStringLiteral("orders.create"), 1);
    target->onRunEnded(QStringLiteral("Failed"));

    const auto handedBack = router.takeHiddenTimeline();

    ASSERT_TRUE(handedBack.has_value());
    TimelineModel orders;
    orders.restoreSnapshot(*handedBack);
    EXPECT_EQ(roleAt(orders, 0, TimelineModel::StatusLabelRole), QStringLiteral("failed"));
    EXPECT_EQ(orders.stepForOperation(QStringLiteral("orders.create")), 2);
    EXPECT_FALSE(router.takeHiddenTimeline().has_value());
}

TEST(RunTabRouter, showing_the_origin_tab_again_moves_the_run_back_into_the_visible_timeline) {
    RunTabRouter router;
    TimelineModel visible;
    const TimelineModel::Snapshot parkedOrders = startOrdersRunThenSwitch(router, visible);
    router.timeline(visible, /*shown=*/false, &parkedOrders)
        ->onStepStarted(1, QStringLiteral("orders.create"), 1);

    TimelineModel* target = router.timeline(visible, /*shown=*/true, &parkedOrders);
    target->onRunEnded(QStringLiteral("Succeeded"));

    EXPECT_EQ(target, &visible);
    EXPECT_EQ(visible.stepForOperation(QStringLiteral("orders.create")), 2);
    EXPECT_EQ(roleAt(visible, 0, TimelineModel::StatusLabelRole), QStringLiteral("done"));
}

TEST(RunTabRouter, a_discarded_run_or_a_vanished_origin_tab_routes_nowhere) {
    RunTabRouter router;
    TimelineModel visible;
    router.begin(kOrders);

    EXPECT_EQ(router.timeline(visible, /*shown=*/false, nullptr), nullptr);
    EXPECT_TRUE(router.originIs(kOrders));
    EXPECT_FALSE(router.originIs(kUsers));
    router.discard();
    EXPECT_EQ(router.timeline(visible, /*shown=*/true, nullptr), nullptr);
    EXPECT_FALSE(router.originIs(kOrders));
}

TEST(RunTabRouter, a_run_without_an_origin_tab_always_uses_the_visible_timeline) {
    RunTabRouter router;
    TimelineModel visible;
    router.begin(RunTabRouter::TabKey{});

    EXPECT_EQ(router.timeline(visible, /*shown=*/false, nullptr), &visible);
}

TEST(RunTabRouter, only_the_target_operations_step_is_the_target_step) {
    RunTabRouter router;
    router.begin(kOrders);
    router.setTarget(QStringLiteral("orders.create"));
    router.noteStep(0, QStringLiteral("auth.login"));
    router.noteStep(1, QStringLiteral("orders.create"));

    EXPECT_FALSE(router.isTargetStep(0));
    EXPECT_TRUE(router.isTargetStep(1));
    EXPECT_FALSE(router.isTargetStep(2));
    EXPECT_EQ(router.stepOperation(0), QStringLiteral("auth.login"));
}

TEST(RunTabRouter, the_panel_takes_only_the_target_operations_own_responses) {
    RunTabRouter router;
    router.begin(kOrders);
    router.setTarget(QStringLiteral("orders.create"));
    router.noteStep(0, QStringLiteral("auth.login"));
    router.noteStep(1, QStringLiteral("orders.create"));

    EXPECT_TRUE(router.showsResponse(1, QStringLiteral("operation")));
    EXPECT_TRUE(router.showsResponse(1, QStringLiteral("poll_attempt")));
    EXPECT_FALSE(router.showsResponse(1, QStringLiteral("sign_in")));
    EXPECT_FALSE(router.showsResponse(1, QStringLiteral("session_refresh")));
    EXPECT_FALSE(router.showsResponse(1, QStringLiteral("inline_auth_token")));
    EXPECT_FALSE(router.showsResponse(0, QStringLiteral("operation")));
}

TEST(RunTabRouter, a_run_follows_its_tab_through_a_rename_of_the_operation) {
    RunTabRouter router;
    router.begin(kOrders);
    router.setTarget(QStringLiteral("orders.create"));
    router.noteStep(0, QStringLiteral("orders.create"));
    const RunTabRouter::TabKey renamed{0, QStringLiteral("orders.place")};

    router.renameOrigin(kUsers, RunTabRouter::TabKey{0, QStringLiteral("users.find")});
    EXPECT_TRUE(router.originIs(kOrders));

    router.renameOrigin(kOrders, renamed);
    EXPECT_TRUE(router.originIs(renamed));
    EXPECT_FALSE(router.originIs(kOrders));
    EXPECT_TRUE(router.isTargetStep(0));
}

TEST(RunTabRouter, nothing_routes_once_the_run_has_ended) {
    RunTabRouter router;
    TimelineModel visible;
    router.begin(kOrders);
    router.setTarget(QStringLiteral("orders.create"));
    router.noteStep(0, QStringLiteral("orders.create"));

    router.end();

    EXPECT_FALSE(router.inFlight());
    EXPECT_EQ(router.timeline(visible, /*shown=*/true, nullptr), nullptr);
    EXPECT_FALSE(router.isTargetStep(0));
}

TEST(TimelineModel, run_error_adds_a_root_failure_row_with_the_engine_code_and_detail) {
    TimelineModel model;

    model.onRunError(QStringLiteral("E_DEPENDENCY_CYCLE"), QStringLiteral("a -> b -> a"));

    ASSERT_EQ(model.rowCount(), 1);
    EXPECT_EQ(model.rootFailureRow(), 0);
    EXPECT_EQ(roleAt(model, 0, TimelineModel::StatusTokenRole), QStringLiteral("error"));
    EXPECT_EQ(roleAt(model, 0, TimelineModel::DetailRole), QStringLiteral("a -> b -> a"));
    EXPECT_TRUE(roleAt(model, 0, TimelineModel::ValueRole).contains("E_DEPENDENCY_CYCLE"));
}

TEST(TimelineModel, run_error_settles_a_header_still_marked_running) {
    TimelineModel model;
    model.onRunStarted(QStringLiteral("orders.create"), 2, QStringLiteral("dev"));

    model.onRunError(QStringLiteral("E_SECRET_ACCESS_FAILED"), QStringLiteral("no keychain"));

    EXPECT_EQ(roleAt(model, 0, TimelineModel::StatusLabelRole), QStringLiteral("failed"));
    EXPECT_EQ(roleAt(model, model.rootFailureRow(), TimelineModel::DetailRole),
              QStringLiteral("no keychain"));
}

TEST(TimelineModel, a_snapshot_shares_captured_bodies_instead_of_copying_them) {
    TimelineModel model;
    model.onRunStarted(QStringLiteral("files.get"), 1, QStringLiteral("dev"));
    model.onStepStarted(0, QStringLiteral("files.get"), 1);
    model.onResponseReceived(0, 200, QString{}, 1 << 20, 5, QString(1 << 20, QLatin1Char('x')));
    const int responseRow = model.rowCount() - 1;
    const QString shown =
        model.data(model.index(responseRow, 0), TimelineModel::ValueRole).toString();

    const TimelineModel::Snapshot first = model.takeSnapshot();
    TimelineModel other;
    other.restoreSnapshot(first);
    const TimelineModel::Snapshot second = other.takeSnapshot();

    EXPECT_EQ(second.rows.back().value.constData(), shown.constData());
}

}  // namespace reqloom::desktop::qml::tests
