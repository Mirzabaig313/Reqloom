// Tests SavedResponseStore: save → list round-trip, overwrite-by-name, remove, persistence across
// instances, the per-user database, the legacy-file import and failure reporting. Temp dirs only;
// no engine, no network.
#include "application/SavedResponseStore.h"

#include <gtest/gtest.h>

#include <QtCore/QByteArray>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QStringList>
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

TEST(SavedResponseStore, rename_operation_moves_its_examples_and_persists) {
    QTemporaryDir dir;
    SavedResponseStore store;
    store.setProjectRoot(dir.path());
    store.save(QStringLiteral("a.b"), makeResponse(QStringLiteral("Success"), 200));

    EXPECT_TRUE(store.renameOperation(QStringLiteral("a.b"), QStringLiteral("a.c")));

    EXPECT_TRUE(store.list(QStringLiteral("a.b")).isEmpty());
    SavedResponseStore reopened;
    reopened.setProjectRoot(dir.path());
    ASSERT_EQ(reopened.list(QStringLiteral("a.c")).size(), 1);
    EXPECT_FALSE(store.renameOperation(QStringLiteral("missing"), QStringLiteral("a.d")));
}

TEST(SavedResponseStore, save_without_project_root_fails) {
    SavedResponseStore store;  // no setProjectRoot
    EXPECT_FALSE(store.save(QStringLiteral("a.b"), makeResponse(QStringLiteral("x"), 200)));
}

namespace {

[[nodiscard]] QByteArray readBytes(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

void writeBytes(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(bytes);
}

[[nodiscard]] QString legacyPath(const QTemporaryDir& project) {
    return QDir(project.path()).filePath(QStringLiteral(".reqloom-responses.json"));
}

const QByteArray kLegacyJson = R"({
  "operations": {
    "auth.login": [
      {"name": "Success", "status": 200, "headers": "", "body": "{\"token\":\"t\"}", "elapsedMs": 5},
      {"name": "Denied", "status": 401, "headers": "", "body": "{}", "elapsedMs": 3}
    ]
  }
})";

}  // namespace

TEST(SavedResponseStore, default_database_lives_in_the_config_dir_override) {
    const QString configDir = qEnvironmentVariable("REQLOOM_CONFIG_DIR");
    ASSERT_FALSE(configDir.isEmpty());
    EXPECT_EQ(QFileInfo(SavedResponseStore::defaultDatabasePath()).absolutePath(),
              QFileInfo(configDir).absoluteFilePath());
}

TEST(SavedResponseStore, saving_writes_nothing_into_the_project_folder) {
    QTemporaryDir project;
    QTemporaryDir data;
    const QString db = QDir(data.path()).filePath(QStringLiteral("examples.db"));
    SavedResponseStore store{db};
    store.setProjectRoot(project.path());

    ASSERT_TRUE(store.save(QStringLiteral("a.b"), makeResponse(QStringLiteral("Success"), 200)));

    EXPECT_TRUE(QDir(project.path())
                    .entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot)
                    .isEmpty());
    EXPECT_TRUE(QFileInfo::exists(db));
}

TEST(SavedResponseStore, legacy_file_is_imported_once_and_left_untouched) {
    QTemporaryDir project;
    QTemporaryDir data;
    const QString db = QDir(data.path()).filePath(QStringLiteral("examples.db"));
    writeBytes(legacyPath(project), kLegacyJson);
    {
        SavedResponseStore store{db};
        store.setProjectRoot(project.path(), QStringLiteral("proj-legacy"));
        const auto report = store.importLegacyFile();
        EXPECT_EQ(report.result, SavedResponseStore::LegacyImport::Result::Imported);
        EXPECT_EQ(report.imported, 2);
        ASSERT_EQ(store.list(QStringLiteral("auth.login")).size(), 2);
        EXPECT_EQ(store.list(QStringLiteral("auth.login")).front().name, QStringLiteral("Success"));
        ASSERT_TRUE(
            store.save(QStringLiteral("auth.login"), makeResponse(QStringLiteral("New"), 201)));
    }
    SavedResponseStore again{db};
    again.setProjectRoot(project.path(), QStringLiteral("proj-legacy"));
    EXPECT_EQ(again.importLegacyFile().result,
              SavedResponseStore::LegacyImport::Result::AlreadyImported);
    EXPECT_EQ(again.list(QStringLiteral("auth.login")).size(), 3);
    EXPECT_EQ(readBytes(legacyPath(project)), kLegacyJson);
}

TEST(SavedResponseStore, unreadable_legacy_file_is_skipped_untouched_and_retried_later) {
    QTemporaryDir project;
    QTemporaryDir data;
    const QString db = QDir(data.path()).filePath(QStringLiteral("examples.db"));
    const QByteArray conflicted = "<<<<<<< HEAD\n{\"operations\":{}}\n=======\n";
    writeBytes(legacyPath(project), conflicted);

    SavedResponseStore store{db};
    store.setProjectRoot(project.path(), QStringLiteral("proj-c"));
    EXPECT_EQ(store.importLegacyFile().result,
              SavedResponseStore::LegacyImport::Result::Unreadable);
    EXPECT_EQ(readBytes(legacyPath(project)), conflicted);
    EXPECT_TRUE(store.save(QStringLiteral("a.b"), makeResponse(QStringLiteral("Kept"), 200)));

    writeBytes(legacyPath(project), kLegacyJson);
    EXPECT_EQ(store.importLegacyFile().result, SavedResponseStore::LegacyImport::Result::Imported);
    EXPECT_EQ(store.list(QStringLiteral("a.b")).size(), 1);
    EXPECT_EQ(store.list(QStringLiteral("auth.login")).size(), 2);
}

TEST(SavedResponseStore, missing_legacy_file_reports_nothing_to_import) {
    QTemporaryDir project;
    QTemporaryDir data;
    SavedResponseStore store{QDir(data.path()).filePath(QStringLiteral("examples.db"))};
    store.setProjectRoot(project.path());
    EXPECT_EQ(store.importLegacyFile().result, SavedResponseStore::LegacyImport::Result::None);
}

TEST(SavedResponseStore, corrupt_database_is_reported_and_left_as_it_is) {
    QTemporaryDir project;
    QTemporaryDir data;
    const QString db = QDir(data.path()).filePath(QStringLiteral("examples.db"));
    const QByteArray garbage(4096, 'x');
    writeBytes(db, garbage);

    QStringList reported;
    SavedResponseStore store{db};
    store.setErrorHandler([&reported](const QString& message) { reported << message; });
    EXPECT_FALSE(store.setProjectRoot(project.path()));
    EXPECT_FALSE(store.lastError().isEmpty());
    EXPECT_FALSE(store.save(QStringLiteral("a.b"), makeResponse(QStringLiteral("x"), 200)));
    EXPECT_FALSE(reported.isEmpty());
    EXPECT_EQ(readBytes(db), garbage);
}

TEST(SavedResponseStore, a_failed_write_is_reported_and_changes_nothing) {
    QTemporaryDir project;
    QTemporaryDir data;
    const QString db = QDir(data.path()).filePath(QStringLiteral("examples.db"));
    {
        SavedResponseStore seed{db};
        seed.setProjectRoot(project.path());
        ASSERT_TRUE(seed.save(QStringLiteral("a.b"), makeResponse(QStringLiteral("Old"), 200)));
    }
    ASSERT_TRUE(QFile::setPermissions(db, QFileDevice::ReadOwner));

    QStringList reported;
    SavedResponseStore store{db};
    store.setErrorHandler([&reported](const QString& message) { reported << message; });
    store.setProjectRoot(project.path());
    EXPECT_FALSE(store.save(QStringLiteral("a.b"), makeResponse(QStringLiteral("New"), 201)));
    EXPECT_FALSE(reported.isEmpty());
    ASSERT_EQ(store.list(QStringLiteral("a.b")).size(), 1);
    EXPECT_EQ(store.list(QStringLiteral("a.b")).front().name, QStringLiteral("Old"));
    QFile::setPermissions(db, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

TEST(SavedResponseStore, examples_follow_the_project_id_across_folders) {
    QTemporaryDir first;
    QTemporaryDir second;
    QTemporaryDir data;
    const QString db = QDir(data.path()).filePath(QStringLiteral("examples.db"));
    SavedResponseStore store{db};
    store.setProjectRoot(first.path(), QStringLiteral("proj-shared"));
    ASSERT_TRUE(store.save(QStringLiteral("a.b"), makeResponse(QStringLiteral("Success"), 200)));

    SavedResponseStore other{db};
    other.setProjectRoot(second.path(), QStringLiteral("proj-shared"));
    EXPECT_EQ(other.list(QStringLiteral("a.b")).size(), 1);
    other.setProjectRoot(second.path(), QStringLiteral("proj-unrelated"));
    EXPECT_TRUE(other.list(QStringLiteral("a.b")).isEmpty());
}

TEST(SavedResponseStore, examples_saved_before_the_project_had_an_id_move_to_the_id) {
    QTemporaryDir project;
    QTemporaryDir data;
    const QString db = QDir(data.path()).filePath(QStringLiteral("examples.db"));
    {
        SavedResponseStore store{db};
        store.setProjectRoot(project.path());
        ASSERT_TRUE(
            store.save(QStringLiteral("a.b"), makeResponse(QStringLiteral("Success"), 200)));
    }
    SavedResponseStore withId{db};
    withId.setProjectRoot(project.path(), QStringLiteral("proj-late"));
    EXPECT_EQ(withId.list(QStringLiteral("a.b")).size(), 1);
}

TEST(SavedResponseStore, an_oversized_body_is_refused_and_reported) {
    QTemporaryDir project;
    QTemporaryDir data;
    QStringList reported;
    SavedResponseStore store{QDir(data.path()).filePath(QStringLiteral("examples.db"))};
    store.setErrorHandler([&reported](const QString& message) { reported << message; });
    store.setProjectRoot(project.path());
    SavedResponse big = makeResponse(QStringLiteral("Big"), 200);
    big.body = QString(static_cast<qsizetype>(SavedResponseStore::kMaxBodyBytes) + 1, u'a');

    EXPECT_FALSE(store.save(QStringLiteral("a.b"), big));
    EXPECT_FALSE(reported.isEmpty());
    EXPECT_TRUE(store.list(QStringLiteral("a.b")).isEmpty());
}

TEST(SavedResponseStore, clear_operation_and_clear_project_remove_examples) {
    QTemporaryDir project;
    QTemporaryDir data;
    const QString db = QDir(data.path()).filePath(QStringLiteral("examples.db"));
    SavedResponseStore store{db};
    store.setProjectRoot(project.path());
    store.save(QStringLiteral("a.b"), makeResponse(QStringLiteral("One"), 200));
    store.save(QStringLiteral("a.c"), makeResponse(QStringLiteral("Two"), 200));

    EXPECT_TRUE(store.clearOperation(QStringLiteral("a.b")));
    EXPECT_TRUE(store.list(QStringLiteral("a.b")).isEmpty());
    EXPECT_EQ(store.list(QStringLiteral("a.c")).size(), 1);

    EXPECT_TRUE(store.clearProject());
    SavedResponseStore reopened{db};
    reopened.setProjectRoot(project.path());
    EXPECT_TRUE(reopened.operationIds().isEmpty());
}

TEST(SavedResponseStore, rename_operation_keeps_both_examples_when_names_clash) {
    QTemporaryDir project;
    QTemporaryDir data;
    SavedResponseStore store{QDir(data.path()).filePath(QStringLiteral("examples.db"))};
    store.setProjectRoot(project.path());
    store.save(QStringLiteral("a.b"), makeResponse(QStringLiteral("Success"), 200));
    store.save(QStringLiteral("a.c"), makeResponse(QStringLiteral("Success"), 201));

    ASSERT_TRUE(store.renameOperation(QStringLiteral("a.b"), QStringLiteral("a.c")));

    const auto list = store.list(QStringLiteral("a.c"));
    ASSERT_EQ(list.size(), 2);
    EXPECT_EQ(list.front().status, 201);
    EXPECT_NE(list.back().name, list.front().name);
}

}  // namespace reqloom::desktop::tests
