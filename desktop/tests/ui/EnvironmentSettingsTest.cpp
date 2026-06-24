// Tests for EnvironmentSettings — the per-project active-environment
// persistence the toolbar uses. Each test runs against an isolated
// temp-file-backed QSettings so it never touches the real user store.
#include "application/EnvironmentSettings.h"

#include <gtest/gtest.h>

#include <QtCore/QSettings>
#include <QtCore/QString>
#include <QtCore/QTemporaryDir>

namespace reqloom::desktop::tests {

namespace {

/// A QSettings backed by an INI file inside a unique temp directory, so
/// reads/writes are isolated from the real application settings and from
/// other tests. The file path is handed to QSettings but never pre-created:
/// QSettings owns and creates it. This matters on Windows, where QSettings'
/// IniFormat sync() writes by atomically renaming a scratch file over the
/// target — replacing a file created/held by something else (e.g. a
/// QTemporaryFile handle) fails there. Letting QSettings own the file avoids
/// that entirely. The QTemporaryDir removes the file (and dir) on teardown.
class ScopedSettings {
public:
    [[nodiscard]] bool ok() const { return dir_.isValid(); }

    [[nodiscard]] QSettings make() const {
        return QSettings{dir_.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat};
    }

private:
    QTemporaryDir dir_;
};

}  // namespace

TEST(EnvironmentSettings, save_then_load_round_trips_per_project) {
    ScopedSettings store;
    ASSERT_TRUE(store.ok());
    {
        auto s = store.make();
        EnvironmentSettings::save(s, QStringLiteral("/projects/alpha"), QStringLiteral("staging"));
    }
    auto s = store.make();
    EXPECT_EQ(EnvironmentSettings::load(s, QStringLiteral("/projects/alpha")),
              QStringLiteral("staging"));
}

TEST(EnvironmentSettings, distinct_projects_keep_independent_selections) {
    ScopedSettings store;
    auto s = store.make();
    EnvironmentSettings::save(s, QStringLiteral("/projects/alpha"), QStringLiteral("prod"));
    EnvironmentSettings::save(s, QStringLiteral("/projects/beta"), QStringLiteral("local"));

    EXPECT_EQ(EnvironmentSettings::load(s, QStringLiteral("/projects/alpha")),
              QStringLiteral("prod"));
    EXPECT_EQ(EnvironmentSettings::load(s, QStringLiteral("/projects/beta")),
              QStringLiteral("local"));
}

TEST(EnvironmentSettings, load_unknown_project_returns_empty) {
    ScopedSettings store;
    auto s = store.make();
    EXPECT_TRUE(EnvironmentSettings::load(s, QStringLiteral("/never/saved")).isEmpty());
}

TEST(EnvironmentSettings, save_overwrites_previous_selection) {
    ScopedSettings store;
    auto s = store.make();
    EnvironmentSettings::save(s, QStringLiteral("/projects/alpha"), QStringLiteral("staging"));
    EnvironmentSettings::save(s, QStringLiteral("/projects/alpha"), QStringLiteral("prod"));

    EXPECT_EQ(EnvironmentSettings::load(s, QStringLiteral("/projects/alpha")),
              QStringLiteral("prod"));
}

TEST(EnvironmentSettings, empty_key_or_value_is_ignored) {
    ScopedSettings store;
    auto s = store.make();
    // Empty project key — nothing stored, nothing returned.
    EnvironmentSettings::save(s, QString{}, QStringLiteral("staging"));
    EXPECT_TRUE(EnvironmentSettings::load(s, QString{}).isEmpty());

    // Empty env value must not clobber an existing selection.
    EnvironmentSettings::save(s, QStringLiteral("/projects/alpha"), QStringLiteral("prod"));
    EnvironmentSettings::save(s, QStringLiteral("/projects/alpha"), QString{});
    EXPECT_EQ(EnvironmentSettings::load(s, QStringLiteral("/projects/alpha")),
              QStringLiteral("prod"));
}

}  // namespace reqloom::desktop::tests
