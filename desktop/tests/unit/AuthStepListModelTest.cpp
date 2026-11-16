// AuthStepListModel: the N-step login-chain editor model. Verifies rebuild
// seeding, per-field mutation with change guards, add/remove (last-step guard),
// adjacent reorder, and that each row owns a usable nested extraction model.
#include "AuthStepListModel.h"

#include <gtest/gtest.h>

#include <QtCore/QModelIndex>

using reqloom::desktop::qml::AuthStepListModel;
using reqloom::desktop::qml::EditableKeyValueModel;

namespace {

AuthStepListModel::StepSeed makeSeed(const QString& id,
                                     const QString& method,
                                     const QString& path) {
    AuthStepListModel::StepSeed seed;
    seed.id = id;
    seed.method = method;
    seed.path = path;
    return seed;
}

TEST(AuthStepListModel, rebuild_seeds_rows_and_exposes_fields) {
    AuthStepListModel model;
    AuthStepListModel::StepSeed one = makeSeed("send", "POST", "/send-otp");
    one.body = QStringLiteral("{\"phone\":\"x\"}");
    one.expect = QStringLiteral("200");
    one.extractions = {{QStringLiteral("challenge"), QStringLiteral("data.challengeId")}};
    AuthStepListModel::StepSeed two = makeSeed("verify", "POST", "/verify-otp");

    model.rebuild({one, two});

    ASSERT_EQ(model.count(), 2);
    EXPECT_EQ(model.methodAt(0), QStringLiteral("POST"));
    EXPECT_EQ(model.pathAt(0), QStringLiteral("/send-otp"));
    EXPECT_EQ(model.bodyAt(0), QStringLiteral("{\"phone\":\"x\"}"));
    EXPECT_EQ(model.expectAt(0), QStringLiteral("200"));
    EXPECT_EQ(model.idAt(1), QStringLiteral("verify"));
    // Nested extraction model carries the seeded pair (+ trailing ghost row).
    const EditableKeyValueModel* ex = model.extractModelAt(0);
    ASSERT_NE(ex, nullptr);
    const auto pairs = ex->pairs();
    ASSERT_EQ(pairs.size(), 1u);
    EXPECT_EQ(pairs.front().first, QStringLiteral("challenge"));
}

TEST(EditableKeyValueModel, clearing_non_trailing_row_removes_it_and_keeps_ghost) {
    EditableKeyValueModel model;
    model.setPairs({{QStringLiteral("token"), QStringLiteral("value")}});

    model.setKey(0, QString{});
    model.setValue(0, QString{});
    model.pruneBlankRows();

    EXPECT_TRUE(model.pairs().empty());
    ASSERT_EQ(model.rowCount(), 1);
    EXPECT_TRUE(model.data(model.index(0), EditableKeyValueModel::IsGhostRole).toBool());
}

TEST(EditableKeyValueModel, row_cleared_while_typing_stays_until_pruned) {
    EditableKeyValueModel model;
    model.setKey(0, QStringLiteral("x"));
    ASSERT_EQ(model.rowCount(), 2);

    model.setKey(0, QString{});

    ASSERT_EQ(model.rowCount(), 2);
    EXPECT_FALSE(model.data(model.index(0), EditableKeyValueModel::IsGhostRole).toBool());
    EXPECT_TRUE(model.data(model.index(1), EditableKeyValueModel::IsGhostRole).toBool());
    EXPECT_TRUE(model.pairs().empty());
}

TEST(EditableKeyValueModel, prune_drops_every_blank_row_and_keeps_one_ghost) {
    EditableKeyValueModel model;
    model.setPairs({{QStringLiteral("a"), QStringLiteral("1")},
                    {QStringLiteral("b"), QStringLiteral("2")},
                    {QStringLiteral("c"), QStringLiteral("3")}});
    model.setKey(0, QString{});
    model.setValue(0, QString{});
    model.setKey(2, QString{});
    model.setValue(2, QString{});
    ASSERT_EQ(model.rowCount(), 4);

    model.pruneBlankRows();

    ASSERT_EQ(model.rowCount(), 2);
    EXPECT_EQ(model.data(model.index(0), EditableKeyValueModel::KeyRole).toString(),
              QStringLiteral("b"));
    EXPECT_TRUE(model.data(model.index(1), EditableKeyValueModel::IsGhostRole).toBool());
}

TEST(EditableKeyValueModel, prune_keeps_the_row_that_still_has_focus) {
    EditableKeyValueModel model;
    model.setPairs(
        {{QStringLiteral("a"), QStringLiteral("1")}, {QStringLiteral("b"), QStringLiteral("2")}});
    model.setKey(0, QString{});
    model.setValue(0, QString{});
    model.setKey(1, QString{});
    model.setValue(1, QString{});

    model.pruneBlankRows(1);

    ASSERT_EQ(model.rowCount(), 2);
    EXPECT_FALSE(model.data(model.index(0), EditableKeyValueModel::IsGhostRole).toBool());
    EXPECT_TRUE(model.data(model.index(1), EditableKeyValueModel::IsGhostRole).toBool());
}

TEST(AuthStepListModel, setters_update_fields_via_data_roles) {
    AuthStepListModel model;
    model.rebuild({makeSeed("login", "POST", "/login")});

    model.setMethod(0, QStringLiteral("GET"));
    model.setPath(0, QStringLiteral("/auth"));
    model.setBody(0, QStringLiteral("{}"));
    model.setExpect(0, QStringLiteral("201"));

    const QModelIndex idx = model.index(0);
    EXPECT_EQ(model.data(idx, AuthStepListModel::MethodRole).toString(), QStringLiteral("GET"));
    EXPECT_EQ(model.data(idx, AuthStepListModel::PathRole).toString(), QStringLiteral("/auth"));
    EXPECT_EQ(model.data(idx, AuthStepListModel::BodyRole).toString(), QStringLiteral("{}"));
    EXPECT_EQ(model.data(idx, AuthStepListModel::ExpectRole).toString(), QStringLiteral("201"));
}

TEST(AuthStepListModel, add_step_appends_blank_post_step) {
    AuthStepListModel model;
    model.rebuild({makeSeed("login", "POST", "/login")});

    const int at = model.addStep();

    EXPECT_EQ(at, 1);
    ASSERT_EQ(model.count(), 2);
    EXPECT_EQ(model.methodAt(1), QStringLiteral("POST"));
    EXPECT_TRUE(model.pathAt(1).isEmpty());
    EXPECT_NE(model.extractModelAt(1), nullptr);
}

TEST(AuthStepListModel, remove_step_keeps_at_least_one) {
    AuthStepListModel model;
    model.rebuild({makeSeed("a", "POST", "/a"), makeSeed("b", "POST", "/b")});

    model.removeStep(0);
    ASSERT_EQ(model.count(), 1);
    EXPECT_EQ(model.idAt(0), QStringLiteral("b"));

    // The last remaining step is not removable.
    model.removeStep(0);
    EXPECT_EQ(model.count(), 1);
}

TEST(AuthStepListModel, move_step_swaps_adjacent_rows) {
    AuthStepListModel model;
    model.rebuild({makeSeed("a", "POST", "/a"), makeSeed("b", "GET", "/b")});

    model.moveStep(0, 1);
    EXPECT_EQ(model.idAt(0), QStringLiteral("b"));
    EXPECT_EQ(model.idAt(1), QStringLiteral("a"));

    // Out-of-range move is a no-op.
    model.moveStep(1, 1);
    EXPECT_EQ(model.idAt(0), QStringLiteral("b"));
    EXPECT_EQ(model.idAt(1), QStringLiteral("a"));
}

TEST(AuthStepListModel, every_edit_including_a_step_extraction_reports_steps_edited) {
    AuthStepListModel model;
    model.rebuild({makeSeed("a", "POST", "/a"), makeSeed("b", "GET", "/b")});
    int edits = 0;
    QObject::connect(&model, &AuthStepListModel::stepsEdited, &model, [&edits]() { ++edits; });
    const auto reports = [&edits](auto action) {
        const int before = edits;
        action();
        return edits > before;
    };

    EXPECT_TRUE(reports([&model]() { model.setPath(0, QStringLiteral("/a2")); }));
    EXPECT_TRUE(reports([&model]() { model.setMethod(0, QStringLiteral("PUT")); }));
    EXPECT_TRUE(reports([&model]() { model.setBody(0, QStringLiteral("{}")); }));
    EXPECT_TRUE(reports([&model]() { model.setExpect(0, QStringLiteral("201")); }));
    EXPECT_TRUE(reports([&model]() { model.moveStep(0, 1); }));
    EXPECT_TRUE(reports([&model]() { model.extractModelAt(0)->setKey(0, QStringLiteral("id")); }));
    int added = -1;
    EXPECT_TRUE(reports([&model, &added]() { added = model.addStep(); }));
    EXPECT_TRUE(reports(
        [&model, added]() { model.extractModelAt(added)->setValue(0, QStringLiteral("x")); }));
    EXPECT_TRUE(reports([&model, added]() { model.removeStep(added); }));

    EXPECT_FALSE(reports([&model]() { model.setPath(0, model.pathAt(0)); }));
    EXPECT_FALSE(reports([&model]() { model.rebuild({makeSeed("c", "POST", "/c")}); }));
    EXPECT_TRUE(reports([&model]() { model.extractModelAt(0)->setKey(0, QStringLiteral("id")); }));
}

}  // namespace
