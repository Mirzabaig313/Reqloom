// Tests the command-palette fuzzy matcher: subsequence matching plus ranking
// that favours contiguous and word-boundary hits. Pure functions, no widgets.
#include "widgets/FuzzyMatch.h"

#include <gtest/gtest.h>

namespace chainapi::desktop::widgets::fuzzy::tests {

TEST(FuzzyMatch, empty_query_matches_anything) {
    EXPECT_TRUE(matches(QString{}, QStringLiteral("order.create")));
    EXPECT_EQ(score(QString{}, QStringLiteral("order.create")), 0);
}

TEST(FuzzyMatch, subsequence_matches_in_order) {
    EXPECT_TRUE(matches(QStringLiteral("ordcr"), QStringLiteral("order.create")));
    EXPECT_TRUE(matches(QStringLiteral("oc"), QStringLiteral("order.create")));
    EXPECT_FALSE(matches(QStringLiteral("zzz"), QStringLiteral("order.create")));
    // Out-of-order characters must not match.
    EXPECT_FALSE(matches(QStringLiteral("rco"), QStringLiteral("oc")));
}

TEST(FuzzyMatch, matching_is_case_insensitive) {
    EXPECT_TRUE(matches(QStringLiteral("ORDER"), QStringLiteral("order.create")));
    EXPECT_GT(score(QStringLiteral("ORDER"), QStringLiteral("order.create")), 0);
}

TEST(FuzzyMatch, no_match_scores_negative) {
    EXPECT_LT(score(QStringLiteral("zzz"), QStringLiteral("order.create")), 0);
}

TEST(FuzzyMatch, prefix_outranks_late_match) {
    // "ord" at the start should beat the same letters buried mid-string.
    const int front = score(QStringLiteral("ord"), QStringLiteral("order.list"));
    const int buried = score(QStringLiteral("ord"), QStringLiteral("vendor.update"));
    EXPECT_GT(front, buried);
}

TEST(FuzzyMatch, word_boundary_after_dot_outranks_mid_word) {
    // "create" should rank the op-name segment after the dot above an
    // incidental mid-word occurrence.
    const int boundary = score(QStringLiteral("create"), QStringLiteral("order.create"));
    const int midword = score(QStringLiteral("create"), QStringLiteral("recreatething.get"));
    EXPECT_GT(boundary, midword);
}

}  // namespace chainapi::desktop::widgets::fuzzy::tests
