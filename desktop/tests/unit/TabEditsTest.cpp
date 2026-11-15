// Unsaved-edit rules for editor tabs: dirty tracking, rename/delete propagation, tab restore and
// the unsaved-changes prompt.
#include "models/TabEdits.h"
#include "models/TabModel.h"

#include <gtest/gtest.h>

#include <QtCore/QString>

#include <cstdint>
#include <memory>
#include <vector>

namespace reqloom::desktop::tests {

using qml::TabModel;
using qml::TabState;

namespace {

TabState editingTab(const QString& id) {
    TabState tab;
    tab.id = id;
    tab.module = id.section('.', 0, 0);
    tab.opName = id.section('.', 1);
    tab.title = tab.opName;
    tab.editing = true;
    tab.editMethod = QStringLiteral("GET");
    tab.editPath = QStringLiteral("/users");
    tab.editHeaders = {{QStringLiteral("Accept"), QStringLiteral("application/json")}};
    tab.editDependencies = {QStringLiteral("auth.login")};
    qml::ChainEditorModel::OpSeed seed;
    seed.operationId = id;
    seed.isTarget = true;
    seed.dependencies = {"auth.login"};
    tab.chainSeeds = {seed};
    tab.chainSnapshotValid = true;
    tab.editBaseline = std::make_shared<const TabState>(tab);
    return tab;
}

TabState blankDraft() {
    TabState tab;
    tab.id = QStringLiteral("users.__new_endpoint__");
    tab.module = QStringLiteral("users");
    tab.operationDraft = true;
    tab.editing = true;
    tab.editMethod = QStringLiteral("GET");
    tab.editBaseline = std::make_shared<const TabState>(tab);
    return tab;
}

}  // namespace

TEST(TabEdits, entering_edit_mode_without_changes_is_not_dirty) {
    EXPECT_FALSE(qml::hasUnsavedEdits(editingTab(QStringLiteral("users.list"))));
}

TEST(TabEdits, a_changed_field_is_dirty) {
    TabState tab = editingTab(QStringLiteral("users.list"));
    tab.editPath = QStringLiteral("/users/1");
    EXPECT_TRUE(qml::hasUnsavedEdits(tab));
}

TEST(TabEdits, a_field_changed_back_is_not_dirty) {
    TabState tab = editingTab(QStringLiteral("users.list"));
    tab.editAuthToken = QStringLiteral("secret");
    ASSERT_TRUE(qml::hasUnsavedEdits(tab));
    tab.editAuthToken.clear();
    EXPECT_FALSE(qml::hasUnsavedEdits(tab));
}

TEST(TabEdits, a_changed_chain_step_is_dirty) {
    TabState tab = editingTab(QStringLiteral("users.list"));
    tab.chainSeeds.front().forEachOver = QStringLiteral("orgs");
    EXPECT_TRUE(qml::hasUnsavedEdits(tab));
}

TEST(TabEdits, new_chain_candidates_and_an_acquired_oauth2_token_are_not_edits) {
    TabState tab = editingTab(QStringLiteral("users.list"));
    tab.chainSeeds.front().candidates = {QStringLiteral("users.create")};
    tab.editAuthOauth2AccessToken = QStringLiteral("token");
    EXPECT_FALSE(qml::hasUnsavedEdits(tab));
}

TEST(TabEdits, a_tab_out_of_edit_mode_is_not_dirty) {
    TabState tab = editingTab(QStringLiteral("users.list"));
    tab.editPath = QStringLiteral("/changed");
    tab.editing = false;
    EXPECT_FALSE(qml::hasUnsavedEdits(tab));
}

TEST(TabEdits, a_blank_endpoint_draft_is_not_dirty) {
    EXPECT_FALSE(qml::hasUnsavedEdits(blankDraft()));
}

TEST(TabEdits, an_endpoint_draft_is_dirty_once_named_or_filled) {
    TabState named = blankDraft();
    named.opName = QStringLiteral("create");
    EXPECT_TRUE(qml::hasUnsavedEdits(named));

    TabState filled = blankDraft();
    filled.editBody = QStringLiteral("{}");
    EXPECT_TRUE(qml::hasUnsavedEdits(filled));
}

TEST(TabEdits, a_never_saved_actor_is_dirty_and_a_saved_one_is_not) {
    TabState draft;
    draft.kind = TabState::Kind::Actor;
    EXPECT_TRUE(qml::hasUnsavedEdits(draft));

    TabState saved = draft;
    saved.id = QStringLiteral("admin");
    EXPECT_FALSE(qml::hasUnsavedEdits(saved));
}

TEST(TabEdits, rename_repoints_the_operation_tab_and_keeps_its_edits) {
    TabState tab = editingTab(QStringLiteral("users.list"));
    tab.editBody = QStringLiteral("draft body");

    EXPECT_TRUE(
        qml::renameOperationInTab(tab, QStringLiteral("users.list"), QStringLiteral("people.all")));

    EXPECT_EQ(tab.id, QStringLiteral("people.all"));
    EXPECT_EQ(tab.module, QStringLiteral("people"));
    EXPECT_EQ(tab.opName, QStringLiteral("all"));
    EXPECT_EQ(tab.title, QStringLiteral("all"));
    EXPECT_EQ(tab.editBody, QStringLiteral("draft body"));
    EXPECT_EQ(tab.chainSeeds.front().operationId, QStringLiteral("people.all"));
    EXPECT_TRUE(qml::hasUnsavedEdits(tab));
}

TEST(TabEdits, rename_of_a_dependency_rewrites_live_and_baseline_so_the_tab_stays_clean) {
    TabState tab = editingTab(QStringLiteral("users.list"));

    EXPECT_TRUE(qml::renameOperationInTab(
        tab, QStringLiteral("auth.login"), QStringLiteral("auth.signin")));

    EXPECT_EQ(tab.id, QStringLiteral("users.list"));
    EXPECT_EQ(tab.editDependencies, std::vector<QString>{QStringLiteral("auth.signin")});
    EXPECT_EQ(tab.chainSeeds.front().dependencies, std::vector<std::string>{"auth.signin"});
    EXPECT_FALSE(qml::hasUnsavedEdits(tab));
}

TEST(TabEdits, module_rename_moves_an_endpoint_draft_with_it) {
    TabState draft = blankDraft();

    EXPECT_TRUE(qml::renameOperationInTab(
        draft, QStringLiteral("users.list"), QStringLiteral("people.list")));

    EXPECT_EQ(draft.module, QStringLiteral("people"));
    EXPECT_EQ(draft.id, QStringLiteral("people.__new_endpoint__"));
    EXPECT_TRUE(draft.operationDraft);
}

TEST(TabEdits, rename_of_an_unrelated_operation_changes_nothing) {
    TabState tab = editingTab(QStringLiteral("users.list"));
    EXPECT_FALSE(qml::renameOperationInTab(
        tab, QStringLiteral("orders.list"), QStringLiteral("orders.all")));
}

TEST(TabEdits, a_deleted_dependency_leaves_live_and_baseline_edits) {
    TabState tab = editingTab(QStringLiteral("users.list"));

    EXPECT_TRUE(qml::forgetOperationInTab(tab, QStringLiteral("auth.login")));

    EXPECT_TRUE(tab.editDependencies.empty());
    EXPECT_TRUE(tab.chainSeeds.front().dependencies.empty());
    EXPECT_FALSE(qml::hasUnsavedEdits(tab));
}

TEST(TabEdits, a_deleted_operation_with_edits_becomes_a_dirty_endpoint_draft) {
    TabState tab = editingTab(QStringLiteral("users.list"));
    tab.editBody = QStringLiteral("draft body");

    qml::detachDeletedOperation(tab, QStringLiteral("users.__new_endpoint__"));

    EXPECT_TRUE(tab.operationDraft);
    EXPECT_EQ(tab.id, QStringLiteral("users.__new_endpoint__"));
    EXPECT_EQ(tab.opName, QStringLiteral("list"));
    EXPECT_EQ(tab.editBody, QStringLiteral("draft body"));
    EXPECT_EQ(tab.chainSeeds.front().operationId, QStringLiteral("users.__new_endpoint__"));
    EXPECT_TRUE(qml::hasUnsavedEdits(tab));
}

TEST(TabEdits, restore_plan_counts_the_tabs_that_cannot_reopen) {
    const std::vector<qml::SavedTab> saved{{0, QStringLiteral("users.list")},
                                           {0, QStringLiteral("users.gone")},
                                           {1, QStringLiteral("admin")},
                                           {0, QStringLiteral("malformed")}};

    const auto plan = qml::planTabRestore(saved, [](const qml::SavedTab& tab) {
        return tab.id == QStringLiteral("users.list") || tab.id == QStringLiteral("admin");
    });

    ASSERT_EQ(plan.reopen.size(), 2U);
    EXPECT_EQ(plan.reopen[0].id, QStringLiteral("users.list"));
    EXPECT_EQ(plan.reopen[1].id, QStringLiteral("admin"));
    EXPECT_EQ(plan.dropped, 2);
    EXPECT_TRUE(qml::droppedTabsMessage(plan.dropped).startsWith(QStringLiteral("2 saved tabs")));
}

TEST(TabEdits, prompt_for_one_tab_names_it_and_offers_save_discard_cancel) {
    const auto prompt = qml::unsavedPrompt(
        {QStringLiteral("list")}, qml::UnsavedReason::CloseTabs, /*cancelsRun=*/false);

    EXPECT_EQ(prompt.title, QStringLiteral("Unsaved changes"));
    EXPECT_EQ(prompt.message,
              QStringLiteral("“list” has unsaved changes. Save them before closing the tab?"));
    EXPECT_EQ(prompt.saveText, QStringLiteral("Save"));
    EXPECT_EQ(prompt.discardText, QStringLiteral("Discard"));
    EXPECT_EQ(prompt.cancelText, QStringLiteral("Cancel"));
}

TEST(TabEdits, prompt_for_several_tabs_gives_count_and_names) {
    const auto prompt = qml::unsavedPrompt({QStringLiteral("list"), QStringLiteral("create")},
                                           qml::UnsavedReason::Quit,
                                           /*cancelsRun=*/false);

    EXPECT_EQ(
        prompt.message,
        QStringLiteral("2 tabs have unsaved changes: list, create. Save them before quitting?"));
    EXPECT_EQ(prompt.saveText, QStringLiteral("Save all"));
    EXPECT_EQ(prompt.discardText, QStringLiteral("Discard all"));
}

TEST(TabEdits, prompt_for_a_running_tab_says_the_run_is_cancelled) {
    const auto prompt = qml::unsavedPrompt(
        {QStringLiteral("list")}, qml::UnsavedReason::CloseTabs, /*cancelsRun=*/true);

    EXPECT_TRUE(
        prompt.message.endsWith(QStringLiteral("Closing also cancels the run in progress.")));
}

TEST(TabEdits, save_in_order_stops_at_the_first_failure) {
    std::vector<std::uint64_t> attempted;
    const bool all = qml::saveInOrder({1, 2, 3}, [&attempted](std::uint64_t uid) {
        attempted.push_back(uid);
        return uid != 2;
    });

    EXPECT_FALSE(all);
    EXPECT_EQ(attempted, (std::vector<std::uint64_t>{1, 2}));
}

TEST(TabEdits, chain_status_follows_the_tab_on_screen) {
    const QHash<QString, QString> live{{QStringLiteral("users.list"), QStringLiteral("running")}};
    TabState other;
    other.chainStatus = {{QStringLiteral("orders.list"), QStringLiteral("success")}};

    EXPECT_EQ(qml::shownChainStatus(&other, /*activeOwnsRun=*/false, true, live),
              other.chainStatus);
    EXPECT_EQ(qml::shownChainStatus(&other, /*activeOwnsRun=*/true, true, live), live);
    EXPECT_TRUE(qml::shownChainStatus(nullptr, false, /*runHasTab=*/true, live).isEmpty());
    EXPECT_EQ(qml::shownChainStatus(nullptr, false, /*runHasTab=*/false, live), live);
}

TEST(TabModel, keeps_each_tab_uid_across_moves_removals_and_a_re_append) {
    TabModel model;
    model.append(TabState{});
    model.append(TabState{});
    model.append(TabState{});
    const std::uint64_t first = model.stateAt(0).uid;
    const std::uint64_t last = model.stateAt(2).uid;
    ASSERT_NE(first, last);

    model.move(0, 2);
    EXPECT_EQ(model.indexOfUid(first), 2);
    model.removeAt(0);
    EXPECT_EQ(model.indexOfUid(first), 1);

    TabState keep = model.stateAt(model.indexOfUid(last));
    model.clearAll();
    EXPECT_EQ(model.indexOfUid(last), -1);
    model.append(std::move(keep));
    EXPECT_EQ(model.indexOfUid(last), 0);
}

TEST(TabModel, counts_dirty_tabs) {
    TabModel model;
    TabState dirty;
    dirty.dirty = true;
    model.append(dirty);
    model.append(TabState{});
    model.append(dirty);
    EXPECT_EQ(model.dirtyCount(), 2);
}

}  // namespace reqloom::desktop::tests
