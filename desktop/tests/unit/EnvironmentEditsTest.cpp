// When the Manage Environment dialog holds unsaved changes.
#include "models/EnvironmentEdits.h"

#include <gtest/gtest.h>

#include <QtCore/QString>

#include <utility>

namespace reqloom::desktop::tests {

using qml::EnvironmentEdits;
using qml::hasUnsavedEnvironmentEdits;

namespace {

EnvironmentEdits staging() {
    EnvironmentEdits env;
    env.name = QStringLiteral("staging");
    env.baseUrl = QStringLiteral("https://staging.example.com");
    env.variables = {{QStringLiteral("admin_email"), QStringLiteral("a@example.com")},
                     {QStringLiteral("region"), QStringLiteral("eu")}};
    return env;
}

}  // namespace

TEST(EnvironmentEdits, an_unchanged_environment_is_not_dirty) {
    EXPECT_FALSE(hasUnsavedEnvironmentEdits(staging(), staging()));
}

TEST(EnvironmentEdits, a_changed_name_base_url_or_variable_is_dirty) {
    EnvironmentEdits renamed = staging();
    renamed.name = QStringLiteral("stage");
    EXPECT_TRUE(hasUnsavedEnvironmentEdits(staging(), renamed));

    EnvironmentEdits moved = staging();
    moved.baseUrl = QStringLiteral("https://other.example.com");
    EXPECT_TRUE(hasUnsavedEnvironmentEdits(staging(), moved));

    EnvironmentEdits changed = staging();
    changed.variables[1].second = QStringLiteral("us");
    EXPECT_TRUE(hasUnsavedEnvironmentEdits(staging(), changed));

    EnvironmentEdits added = staging();
    added.variables.emplace_back(QStringLiteral("token"), QString{});
    EXPECT_TRUE(hasUnsavedEnvironmentEdits(staging(), added));

    EnvironmentEdits removed = staging();
    removed.variables.pop_back();
    EXPECT_TRUE(hasUnsavedEnvironmentEdits(staging(), removed));
}

TEST(EnvironmentEdits, what_the_save_ignores_is_not_a_change) {
    EnvironmentEdits live = staging();
    live.name = QStringLiteral("  staging ");
    live.baseUrl = QStringLiteral("https://staging.example.com  ");
    std::swap(live.variables[0], live.variables[1]);
    live.variables.emplace_back(QString{}, QStringLiteral("orphan value"));
    live.variables.emplace_back(QStringLiteral("baseUrl"), QStringLiteral("ignored"));
    EXPECT_FALSE(hasUnsavedEnvironmentEdits(staging(), live));
}

TEST(EnvironmentEdits, a_new_environment_is_dirty_once_anything_is_filled_in) {
    const EnvironmentEdits blank;
    EXPECT_FALSE(hasUnsavedEnvironmentEdits(blank, blank));

    EnvironmentEdits named;
    named.name = QStringLiteral("local");
    EXPECT_TRUE(hasUnsavedEnvironmentEdits(blank, named));

    EnvironmentEdits withVariable;
    withVariable.variables = {{QStringLiteral("k"), QStringLiteral("v")}};
    EXPECT_TRUE(hasUnsavedEnvironmentEdits(blank, withVariable));
}

}  // namespace reqloom::desktop::tests
