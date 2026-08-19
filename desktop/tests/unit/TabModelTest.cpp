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

}  // namespace reqloom::desktop::tests
