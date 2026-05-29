// Tests for EnvironmentSettings — the per-project active-environment
// persistence the toolbar uses. Each test runs against an isolated
// temp-file-backed QSettings so it never touches the real user store.
#include "application/EnvironmentSettings.h"

#include <gtest/gtest.h>

#include <QtCore/QSettings>
#include <QtCore/QString>
#include <QtCore/QTemporaryFile>

namespace chainapi::desktop::tests {

namespace {

/// A QSettings backed by a unique temp INI file, so reads/writes are
/// isolated from the real application settings and from other tests.
class ScopedSettings {
public:
    ScopedSettings() {
        file_.setAutoRemove(true);
        // Realise the temp path so QSettings can open it by name.
        opened_ = file_.open();
    }

    [[nodiscard]] bool ok() const { return opened_; }

    [[nodiscard]] QSettings make() const {
        return QSettings{file_.fileName(), QSettings::IniFormat};
    }

private:
    QTemporaryFile file_;
    bool opened_{false};
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

}  // namespace chainapi::desktop::tests
