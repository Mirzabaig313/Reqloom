// Tests SavedResponseStore: save → list round-trip, overwrite-by-name,
// remove, and persistence to disk across instances. Uses a temp dir as the
// project root; no engine, no network.
#include "application/SavedResponseStore.h"

#include <gtest/gtest.h>

#include <QtCore/QTemporaryDir>

namespace reqloom::desktop::tests {

namespace {

[[nodiscard]] SavedResponse makeResponse(const QString& name, int status) {
    SavedResponse r;
    r.name = name;
    r.status = status;
    r.headers = QStringLiteral("Content-Type: application/json");
    r.body = QStringLiteral(R"({"ok":true})");
    r.elapsedMs = 12;
    return r;
}

}  // namespace

TEST(SavedResponseStore, save_then_list_round_trips) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    SavedResponseStore store;
    store.setProjectRoot(dir.path());
    ASSERT_TRUE(
        store.save(QStringLiteral("auth.login"), makeResponse(QStringLiteral("Success"), 200)));

    const auto list = store.list(QStringLiteral("auth.login"));
    ASSERT_EQ(list.size(), 1);
    EXPECT_EQ(list.front().name, QStringLiteral("Success"));
    EXPECT_EQ(list.front().status, 200);
    EXPECT_EQ(list.front().body, QStringLiteral(R"({"ok":true})"));
}

TEST(SavedResponseStore, saving_same_name_overwrites_not_appends) {
    QTemporaryDir dir;
    SavedResponseStore store;
    store.setProjectRoot(dir.path());
    store.save(QStringLiteral("auth.login"), makeResponse(QStringLiteral("Success"), 200));
    store.save(QStringLiteral("auth.login"), makeResponse(QStringLiteral("Success"), 201));

    const auto list = store.list(QStringLiteral("auth.login"));
    ASSERT_EQ(list.size(), 1);
    EXPECT_EQ(list.front().status, 201);
}

TEST(SavedResponseStore, persists_across_instances_for_same_project) {
    QTemporaryDir dir;
    {
        SavedResponseStore writer;
        writer.setProjectRoot(dir.path());
        writer.save(QStringLiteral("cart.add_item"), makeResponse(QStringLiteral("Created"), 201));
    }
    SavedResponseStore reader;
    reader.setProjectRoot(dir.path());
    const auto list = reader.list(QStringLiteral("cart.add_item"));
    ASSERT_EQ(list.size(), 1);
    EXPECT_EQ(list.front().name, QStringLiteral("Created"));
}

TEST(SavedResponseStore, remove_drops_the_named_example) {
    QTemporaryDir dir;
    SavedResponseStore store;
    store.setProjectRoot(dir.path());
    store.save(QStringLiteral("auth.login"), makeResponse(QStringLiteral("Success"), 200));
    store.save(QStringLiteral("auth.login"), makeResponse(QStringLiteral("Denied"), 401));

    store.remove(QStringLiteral("auth.login"), QStringLiteral("Denied"));

    const auto list = store.list(QStringLiteral("auth.login"));
    ASSERT_EQ(list.size(), 1);
    EXPECT_EQ(list.front().name, QStringLiteral("Success"));
}

TEST(SavedResponseStore, rename_changes_the_name_but_rejects_clashes) {
    QTemporaryDir dir;
    SavedResponseStore store;
    store.setProjectRoot(dir.path());
    store.save(QStringLiteral("a.b"), makeResponse(QStringLiteral("Success"), 200));
    store.save(QStringLiteral("a.b"), makeResponse(QStringLiteral("Denied"), 401));

    EXPECT_TRUE(
        store.rename(QStringLiteral("a.b"), QStringLiteral("Success"), QStringLiteral("OK")));
    // Clashing with an existing name is refused.
    EXPECT_FALSE(
        store.rename(QStringLiteral("a.b"), QStringLiteral("OK"), QStringLiteral("Denied")));

    const auto list = store.list(QStringLiteral("a.b"));
    ASSERT_EQ(list.size(), 2);
    EXPECT_NE(store.list(QStringLiteral("a.b")).front().name, QStringLiteral("Success"));
}

TEST(SavedResponseStore, duplicate_creates_a_uniquely_named_copy) {
    QTemporaryDir dir;
    SavedResponseStore store;
    store.setProjectRoot(dir.path());
    store.save(QStringLiteral("a.b"), makeResponse(QStringLiteral("Success"), 200));

    const QString first = store.duplicate(QStringLiteral("a.b"), QStringLiteral("Success"));
    EXPECT_EQ(first, QStringLiteral("Success copy"));
    const QString second = store.duplicate(QStringLiteral("a.b"), QStringLiteral("Success"));
    EXPECT_EQ(second, QStringLiteral("Success copy 2"));
    EXPECT_EQ(store.list(QStringLiteral("a.b")).size(), 3);
}

TEST(SavedResponseStore, save_without_project_root_fails) {
    SavedResponseStore store;  // no setProjectRoot
    EXPECT_FALSE(store.save(QStringLiteral("a.b"), makeResponse(QStringLiteral("x"), 200)));
}

}  // namespace reqloom::desktop::tests
