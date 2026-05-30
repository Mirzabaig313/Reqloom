// Tests the StatusBadge glyph vocabulary. The accessibility contract (§6.1)
// is that status is never colour alone — so every status must map to a
// distinct, non-empty glyph. This locks that invariant.
#include "widgets/StatusBadge.h"

#include <gtest/gtest.h>

#include <QtCore/QSet>
#include <QtCore/QString>

namespace chainapi::desktop::widgets::tests {

namespace {

using theming::StatusToken;

constexpr StatusToken kAllStatuses[] = {
    StatusToken::Idle,
    StatusToken::Running,
    StatusToken::Success,
    StatusToken::Warning,
    StatusToken::Error,
    StatusToken::Cancelled,
    StatusToken::Blocked,
    StatusToken::Skipped,
};

}  // namespace

TEST(StatusBadge, every_status_has_a_nonempty_glyph) {
    for (const auto token : kAllStatuses) {
        EXPECT_FALSE(StatusBadge::glyph(token).isEmpty())
            << "status " << static_cast<int>(token) << " has no glyph";
    }
}

TEST(StatusBadge, glyphs_are_distinct_so_colour_is_never_the_only_signal) {
    QSet<QString> seen;
    for (const auto token : kAllStatuses) {
        const QString g = StatusBadge::glyph(token);
        EXPECT_FALSE(seen.contains(g))
            << "glyph '" << g.toStdString() << "' is reused across statuses";
        seen.insert(g);
    }
    EXPECT_EQ(seen.size(), std::size(kAllStatuses));
}

}  // namespace chainapi::desktop::widgets::tests
