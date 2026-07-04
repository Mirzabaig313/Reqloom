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

}  // namespace
