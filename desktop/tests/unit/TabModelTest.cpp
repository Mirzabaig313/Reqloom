// Transient operation-tab state and in-place promotion coverage.
#include "models/TabModel.h"

#include <gtest/gtest.h>

#include <QtCore/QString>

namespace reqloom::desktop::tests {

using qml::TabModel;
using qml::TabState;

TEST(TabModel, inserts_operation_draft_at_requested_position) {
    TabModel model;
    TabState saved;
    saved.id = QStringLiteral("users.list");
    saved.title = QStringLiteral("list");
    model.append(std::move(saved));

    TabState draft;
    draft.id = QStringLiteral("users.__new_endpoint__");
    draft.title = QStringLiteral("New endpoint");
    draft.operationDraft = true;
    draft.dirty = true;
    const int inserted = model.insert(0, std::move(draft));

    ASSERT_EQ(inserted, 0);
    ASSERT_EQ(model.count(), 2);
    EXPECT_TRUE(model.stateAt(0).operationDraft);
    EXPECT_TRUE(model.stateAt(0).dirty);
    EXPECT_EQ(model.stateAt(0).title, QStringLiteral("New endpoint"));
    EXPECT_EQ(model.stateAt(1).id, QStringLiteral("users.list"));
}

TEST(TabModel, promotes_operation_draft_without_changing_its_row) {
    TabModel model;
    TabState draft;
    draft.id = QStringLiteral("users.__new_endpoint__");
    draft.title = QStringLiteral("New endpoint");
    draft.operationDraft = true;
    draft.dirty = true;
    const int draftRow = model.append(std::move(draft));

    TabState& promoted = model.stateAt(draftRow);
    promoted.id = QStringLiteral("users.create");
    promoted.opName = QStringLiteral("create");
    promoted.title = QStringLiteral("create");
    promoted.operationDraft = false;
    promoted.dirty = false;
    model.refreshRow(draftRow);

    ASSERT_EQ(model.count(), 1);
    EXPECT_EQ(draftRow, 0);
    EXPECT_EQ(model.indexOf(TabState::Kind::Operation, QStringLiteral("users.create")), draftRow);
    EXPECT_FALSE(model.stateAt(draftRow).operationDraft);
    EXPECT_FALSE(model.stateAt(draftRow).dirty);
}

namespace {

/// One saved operation tab, as openOperationTab builds it.
TabState operationTab(const QString& module, const QString& opName) {
    TabState tab;
    tab.kind = TabState::Kind::Operation;
    tab.id = module + QLatin1Char('.') + opName;
    tab.module = module;
    tab.opName = opName;
    tab.title = opName;
    return tab;
}

}  // namespace

TEST(TabModel, leaves_a_unique_title_unqualified) {
    TabModel model;
    model.append(operationTab(QStringLiteral("worker"), QStringLiteral("industries")));
    model.append(operationTab(QStringLiteral("worker"), QStringLiteral("preferences")));

    EXPECT_EQ(model.displayTitle(0), QStringLiteral("industries"));
    EXPECT_EQ(model.displayTitle(1), QStringLiteral("preferences"));
}

TEST(TabModel, qualifies_both_titles_when_two_modules_share_an_operation_name) {
    TabModel model;
    model.append(operationTab(QStringLiteral("worker"), QStringLiteral("industries")));
    model.append(operationTab(QStringLiteral("employer"), QStringLiteral("industries")));

    EXPECT_EQ(model.displayTitle(0), QStringLiteral("worker.industries"));
    EXPECT_EQ(model.displayTitle(1), QStringLiteral("employer.industries"));
}

TEST(TabModel, drops_the_qualification_once_the_colliding_tab_closes) {
    TabModel model;
    model.append(operationTab(QStringLiteral("worker"), QStringLiteral("industries")));
    model.append(operationTab(QStringLiteral("employer"), QStringLiteral("industries")));
    ASSERT_EQ(model.displayTitle(0), QStringLiteral("worker.industries"));

    model.removeAt(1);

    EXPECT_EQ(model.displayTitle(0), QStringLiteral("industries"));
}

TEST(TabModel, exposes_the_qualified_title_through_the_title_role) {
    TabModel model;
    model.append(operationTab(QStringLiteral("worker"), QStringLiteral("industries")));
    model.append(operationTab(QStringLiteral("employer"), QStringLiteral("industries")));

    const QVariant title = model.data(model.index(0, 0), TabModel::TitleRole);

    EXPECT_EQ(title.toString(), QStringLiteral("worker.industries"));
}

TEST(TabModel, keeps_an_operation_draft_label_bare_despite_a_collision) {
    TabModel model;
    TabState first = operationTab(QStringLiteral("worker"), QStringLiteral("New endpoint"));
    first.operationDraft = true;
    model.append(std::move(first));
    TabState second = operationTab(QStringLiteral("employer"), QStringLiteral("New endpoint"));
    second.operationDraft = true;
    model.append(std::move(second));

    EXPECT_EQ(model.displayTitle(0), QStringLiteral("New endpoint"));
    EXPECT_EQ(model.displayTitle(1), QStringLiteral("New endpoint"));
}

TEST(TabModel, leaves_an_actor_title_bare_when_an_operation_shares_its_name) {
    TabModel model;
    TabState actor;
    actor.kind = TabState::Kind::Actor;
    actor.id = QStringLiteral("industries");
    actor.title = QStringLiteral("industries");
    model.append(std::move(actor));
    model.append(operationTab(QStringLiteral("worker"), QStringLiteral("industries")));

    // The actor has no module to qualify with; qualifying the operation is
    // enough to tell the two rows apart.
    EXPECT_EQ(model.displayTitle(0), QStringLiteral("industries"));
    EXPECT_EQ(model.displayTitle(1), QStringLiteral("worker.industries"));
}

}  // namespace reqloom::desktop::tests
