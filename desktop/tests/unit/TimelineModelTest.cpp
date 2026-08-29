// TimelineModel — step selection and per-step operation identity, the state the
// chain graph and explorer highlight from.

#include <gtest/gtest.h>

#include "models/TimelineModel.h"
#include "views/Formatting.h"

#include <QtCore/QObject>

#include <cstddef>
#include <limits>
#include <vector>

namespace reqloom::desktop::qml::tests {

namespace {

[[nodiscard]] QString roleAt(const TimelineModel& model, int row, int role) {
    return model.data(model.index(row, 0), role).toString();
}

/// Counts emissions of `selectionChanged`. A plain connect rather than
/// QSignalSpy so the test target needs no Qt6::Test dependency; the lambda is
/// receiver-bound so it disconnects with the counter.
class SelectionCounter {
public:
    explicit SelectionCounter(TimelineModel& model) {
        QObject::connect(
            &model, &TimelineModel::selectionChanged, &context_, [this]() { ++count_; });
    }

    [[nodiscard]] int count() const { return count_; }

private:
    QObject context_;
    int count_{0};
};

/// Drive a two-step run far enough that both step rows exist. Row layout after
/// this returns: 0 run header, 1 step one, 2 step two.
void startTwoStepRun(TimelineModel& model) {
    model.onRunStarted(QStringLiteral("catalog.list"), 2, QStringLiteral("default"));
    model.onStepStarted(0, QStringLiteral("auth.login"), 1);
    model.onStepStarted(1, QStringLiteral("catalog.list"), 1);
}

}  // namespace

TEST(TimelineModel, exposes_operation_id_on_step_rows_only) {
    TimelineModel model;
    startTwoStepRun(model);

    EXPECT_EQ(roleAt(model, 1, TimelineModel::OpRole), QStringLiteral("auth.login"));
    EXPECT_EQ(roleAt(model, 2, TimelineModel::OpRole), QStringLiteral("catalog.list"));
    // The run header is not a step and owns no operation.
    EXPECT_TRUE(roleAt(model, 0, TimelineModel::OpRole).isEmpty());
}

TEST(TimelineModel, records_the_producing_operation_and_variable_on_extraction_rows) {
    TimelineModel model;
    startTwoStepRun(model);
    model.onExtractionCompleted(0,
                                QStringLiteral("auth.login"),
                                QStringLiteral("token"),
                                QStringLiteral("$.access_token"),
                                QStringLiteral("missing"),
                                QString{});

    const int extractionRow = model.rowCount() - 1;
    // Both halves are needed to ask which downstream steps consumed the value.
    EXPECT_EQ(roleAt(model, extractionRow, TimelineModel::OpRole), QStringLiteral("auth.login"));
    EXPECT_EQ(roleAt(model, extractionRow, TimelineModel::VariableNameRole),
              QStringLiteral("token"));
}

TEST(TimelineModel, finds_the_step_that_ran_an_operation) {
    TimelineModel model;
    startTwoStepRun(model);

    EXPECT_EQ(model.stepForOperation(QStringLiteral("auth.login")), 1);
    EXPECT_EQ(model.stepForOperation(QStringLiteral("catalog.list")), 2);
    // A blocked consumer never produced a step row, so there is nothing to jump to.
    EXPECT_EQ(model.stepForOperation(QStringLiteral("orders.pay")), 0);
    EXPECT_EQ(model.stepForOperation(QString{}), 0);
}

TEST(TimelineModel, resolves_selected_operation_from_pinned_step) {
    TimelineModel model;
    startTwoStepRun(model);

    EXPECT_EQ(model.selectedStep(), 0);
    EXPECT_TRUE(model.selectedOperationId().isEmpty());

    model.setSelectedStep(2);

    EXPECT_EQ(model.selectedStep(), 2);
    EXPECT_EQ(model.selectedOperationId(), QStringLiteral("catalog.list"));
}

TEST(TimelineModel, clears_selection_when_pinned_step_has_no_row) {
    TimelineModel model;
    startTwoStepRun(model);
    model.setSelectedStep(1);

    // Step 9 never ran; pinning it must clear rather than strand the inspector.
    model.setSelectedStep(9);

    EXPECT_EQ(model.selectedStep(), 0);
    EXPECT_TRUE(model.selectedOperationId().isEmpty());
}

TEST(TimelineModel, emits_nothing_when_selection_is_reassigned_unchanged) {
    TimelineModel model;
    startTwoStepRun(model);
    model.setSelectedStep(1);

    const SelectionCounter counter{model};
    model.setSelectedStep(1);

    // The early return is what keeps a QML read/write pair from looping.
    EXPECT_EQ(counter.count(), 0);
}

TEST(TimelineModel, drops_selection_on_reset) {
    TimelineModel model;
    startTwoStepRun(model);
    model.setSelectedStep(2);

    const SelectionCounter counter{model};
    model.reset();

    EXPECT_EQ(model.selectedStep(), 0);
    EXPECT_EQ(counter.count(), 1);
}

TEST(TimelineModel, carries_selection_across_snapshot_restore) {
    TimelineModel parked;
    startTwoStepRun(parked);
    parked.setSelectedStep(2);
    const TimelineModel::Snapshot snapshot = parked.takeSnapshot();

    // A second tab's model: its own run, its own selection.
    TimelineModel live;
    live.onRunStarted(QStringLiteral("orders.create"), 1, QStringLiteral("default"));
    live.onStepStarted(0, QStringLiteral("orders.create"), 1);
    live.setSelectedStep(1);
    ASSERT_EQ(live.selectedOperationId(), QStringLiteral("orders.create"));

    live.restoreSnapshot(snapshot);

    EXPECT_EQ(live.selectedStep(), 2);
    EXPECT_EQ(live.selectedOperationId(), QStringLiteral("catalog.list"));
}

TEST(TimelineModel, restoring_a_snapshot_announces_selection_even_at_same_step) {
    TimelineModel parked;
    startTwoStepRun(parked);
    parked.setSelectedStep(1);  // → auth.login
    const TimelineModel::Snapshot snapshot = parked.takeSnapshot();

    TimelineModel live;
    live.onRunStarted(QStringLiteral("orders.create"), 1, QStringLiteral("default"));
    live.onStepStarted(0, QStringLiteral("orders.create"), 1);
    live.setSelectedStep(1);  // → orders.create, same step number

    const SelectionCounter counter{live};
    live.restoreSnapshot(snapshot);

    // Step number is unchanged but the operation under it is not, so a bound
    // highlight would go stale without this signal.
    EXPECT_EQ(live.selectedOperationId(), QStringLiteral("auth.login"));
    EXPECT_GE(counter.count(), 1);
}

TEST(UnresolvedDiagnosticFormatting, preserves_order_and_value_free_evidence) {
    std::vector<engine::UnresolvedVariableDiagnostic> diagnostics;
    diagnostics.push_back(engine::UnresolvedVariableDiagnostic{
        .token = "env.ACCOUNT",
        .useKind = engine::VariableUseKind::NamedQuery,
        .useName = "account_id",
        .cause = engine::UnresolvedVariableCause::EnvironmentValueMissing,
        .sourceKind = engine::VariableSourceKind::Environment,
        .sourceId = "staging",
        .sourceField = "ACCOUNT"});
    diagnostics.push_back(engine::UnresolvedVariableDiagnostic{
        .token = "order.order_id",
        .useKind = engine::VariableUseKind::UrlPath,
        .cause = engine::UnresolvedVariableCause::ExtractionMissing,
        .sourceKind = engine::VariableSourceKind::Extraction,
        .sourceId = "order",
        .sourceField = "order_id",
        .producerOp = engine::OperationId{"order.create"},
        .producerStepIndex = 0u});

    const QVariantList formatted = format::unresolvedDiagnostics(diagnostics);

    ASSERT_EQ(formatted.size(), 2);
    const QVariantMap first = formatted[0].toMap();
    EXPECT_EQ(first.value(QStringLiteral("token")).toString(), QStringLiteral("{{env.ACCOUNT}}"));
    EXPECT_EQ(first.value(QStringLiteral("location")).toString(),
              QStringLiteral("Query parameter “account_id”"));
    EXPECT_EQ(first.value(QStringLiteral("sourceId")).toString(), QStringLiteral("staging"));
    const QVariantMap second = formatted[1].toMap();
    EXPECT_EQ(second.value(QStringLiteral("token")).toString(),
              QStringLiteral("{{order.order_id}}"));
    EXPECT_EQ(second.value(QStringLiteral("producerOperationId")).toString(),
              QStringLiteral("order.create"));
    EXPECT_EQ(second.value(QStringLiteral("producerStep")).toInt(), 1);
}

TEST(TimelineModel, exposes_diagnostics_and_only_proven_actions_on_root_failure) {
    TimelineModel model;
    startTwoStepRun(model);
    std::vector<engine::UnresolvedVariableDiagnostic> evidence{engine::UnresolvedVariableDiagnostic{
        .token = "auth.token",
        .useKind = engine::VariableUseKind::Header,
        .useName = "Authorization",
        .cause = engine::UnresolvedVariableCause::ExtractionMissing,
        .sourceKind = engine::VariableSourceKind::Extraction,
        .sourceId = "auth",
        .sourceField = "token",
        .producerOp = engine::OperationId{"auth.login"},
        .producerStepIndex = 0u}};

    model.onStepFailed(1,
                       QStringLiteral("catalog.list"),
                       QStringLiteral("E_VAR_UNRESOLVED"),
                       QStringLiteral("unresolved"),
                       format::unresolvedDiagnostics(evidence));

    const int failureRow = model.rowForStep(2);
    ASSERT_EQ(model.rootFailureRow(), failureRow);
    EXPECT_TRUE(model.data(model.index(failureRow, 0), TimelineModel::RootFailureRole).toBool());
    const QVariantList diagnostics =
        model.data(model.index(failureRow, 0), TimelineModel::DiagnosticsRole).toList();
    ASSERT_EQ(diagnostics.size(), 1);
    const QVariantMap diagnostic = diagnostics.front().toMap();
    EXPECT_TRUE(diagnostic.value(QStringLiteral("canOpenRequestField")).toBool());
    EXPECT_EQ(diagnostic.value(QStringLiteral("requestField")).toString(),
              QStringLiteral("header"));
    EXPECT_TRUE(diagnostic.value(QStringLiteral("canEditSource")).toBool());
    EXPECT_EQ(diagnostic.value(QStringLiteral("editKind")).toString(),
              QStringLiteral("extraction"));
    EXPECT_TRUE(diagnostic.value(QStringLiteral("canShowProducer")).toBool());
    EXPECT_EQ(diagnostic.value(QStringLiteral("producerStep")).toInt(), 1);
}

TEST(TimelineModel, keeps_inline_auth_diagnostic_informational_without_exact_field_evidence) {
    TimelineModel model;
    startTwoStepRun(model);
    const std::vector<engine::UnresolvedVariableDiagnostic> evidence{
        engine::UnresolvedVariableDiagnostic{
            .token = "secret.token",
            .useKind = engine::VariableUseKind::Auth,
            .useName = "Authorization",
            .cause = engine::UnresolvedVariableCause::SecretValueMissing,
            .sourceKind = engine::VariableSourceKind::Secret,
            .sourceId = "secret",
            .sourceField = "token"}};

    model.onStepFailed(1,
                       QStringLiteral("catalog.list"),
                       QStringLiteral("E_VAR_UNRESOLVED"),
                       QStringLiteral("unresolved"),
                       format::unresolvedDiagnostics(evidence));

    const QVariantList diagnostics =
        model.data(model.index(model.rootFailureRow(), 0), TimelineModel::DiagnosticsRole).toList();
    ASSERT_EQ(diagnostics.size(), 1);
    const QVariantMap diagnostic = diagnostics.front().toMap();
    EXPECT_EQ(diagnostic.value(QStringLiteral("location")).toString(),
              QStringLiteral("Authentication “Authorization”"));
    EXPECT_FALSE(diagnostic.value(QStringLiteral("canOpenRequestField")).toBool());
    EXPECT_TRUE(diagnostic.value(QStringLiteral("requestField")).toString().isEmpty());
}

TEST(UnresolvedDiagnosticFormatting, bounds_indexes_before_one_based_model_conversion) {
    EXPECT_EQ(format::boundedIndex(std::numeric_limits<std::size_t>::max()),
              std::numeric_limits<int>::max() - 1);
}

TEST(TimelineModel, records_blocked_step_with_one_based_cause_and_selection_identity) {
    TimelineModel model;
    model.onRunStarted(QStringLiteral("orders.pay"), 2, QStringLiteral("default"));
    model.onStepStarted(0, QStringLiteral("orders.create"), 1);
    model.onStepFailed(0,
                       QStringLiteral("orders.create"),
                       QStringLiteral("E_HTTP_STATUS"),
                       QStringLiteral("HTTP 500"),
                       {});

    model.onStepBlocked(1, QStringLiteral("orders.pay"), 0);

    const int blockedRow = model.rowForStep(2);
    ASSERT_GE(blockedRow, 0);
    EXPECT_EQ(roleAt(model, blockedRow, TimelineModel::StatusTokenRole), QStringLiteral("blocked"));
    EXPECT_EQ(model.data(model.index(blockedRow, 0), TimelineModel::BlockedByStepRole).toInt(), 1);
    EXPECT_EQ(model.stepForOperation(QStringLiteral("orders.pay")), 2);
    EXPECT_EQ(model.rootFailureRow(), model.rowForStep(1));
}

TEST(TimelineModel, snapshot_restores_diagnostics_blocker_and_root_failure) {
    TimelineModel parked;
    parked.onRunStarted(QStringLiteral("orders.pay"), 2, QStringLiteral("default"));
    parked.onStepStarted(0, QStringLiteral("orders.create"), 1);
    parked.onStepFailed(
        0,
        QStringLiteral("orders.create"),
        QStringLiteral("E_VAR_UNRESOLVED"),
        QStringLiteral("unresolved"),
        QVariantList{QVariantMap{{QStringLiteral("token"), QStringLiteral("{{env.ID}}")}}});
    parked.onStepBlocked(1, QStringLiteral("orders.pay"), 0);
    const TimelineModel::Snapshot snapshot = parked.takeSnapshot();

    TimelineModel restored;
    restored.restoreSnapshot(snapshot);

    EXPECT_EQ(restored.rootFailureRow(), restored.rowForStep(1));
    EXPECT_EQ(
        restored.data(restored.index(restored.rowForStep(1), 0), TimelineModel::DiagnosticsRole)
            .toList()
            .size(),
        1);
    EXPECT_EQ(
        restored.data(restored.index(restored.rowForStep(2), 0), TimelineModel::BlockedByStepRole)
            .toInt(),
        1);
}

}  // namespace reqloom::desktop::qml::tests
