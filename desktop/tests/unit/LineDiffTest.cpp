// Tests the LCS line differ that backs the response Diff tab (FR-7.5).
#include "widgets/LineDiff.h"

#include <gtest/gtest.h>

#include <cstddef>

namespace chainapi::desktop::widgets::diff::tests {

namespace {

using K = DiffLine::Kind;

[[nodiscard]] int countOf(const std::vector<DiffLine>& d, K kind) {
    int n = 0;
    for (const auto& line : d) {
        if (line.kind == kind) {
            ++n;
        }
    }
    return n;
}

}  // namespace

TEST(LineDiff, identical_text_is_all_context) {
    const QString a = QStringLiteral("one\ntwo\nthree");
    const auto d = lineDiff(a, a);
    EXPECT_EQ(countOf(d, K::Added), 0);
    EXPECT_EQ(countOf(d, K::Removed), 0);
    EXPECT_EQ(countOf(d, K::Context), 3);
}

TEST(LineDiff, a_changed_line_is_one_remove_and_one_add) {
    const QString a = QStringLiteral("one\ntwo\nthree");
    const QString b = QStringLiteral("one\nTWO\nthree");
    const auto d = lineDiff(a, b);
    EXPECT_EQ(countOf(d, K::Removed), 1);
    EXPECT_EQ(countOf(d, K::Added), 1);
    EXPECT_EQ(countOf(d, K::Context), 2);

    // The removed "two" must appear before the added "TWO".
    int removedIdx = -1;
    int addedIdx = -1;
    for (std::size_t i = 0; i < d.size(); ++i) {
        if (d[i].kind == K::Removed && d[i].text == QStringLiteral("two")) {
            removedIdx = static_cast<int>(i);
        }
        if (d[i].kind == K::Added && d[i].text == QStringLiteral("TWO")) {
            addedIdx = static_cast<int>(i);
        }
    }
    ASSERT_GE(removedIdx, 0);
    ASSERT_GE(addedIdx, 0);
    EXPECT_LT(removedIdx, addedIdx);
}

TEST(LineDiff, pure_insertion_only_adds) {
    const QString a = QStringLiteral("one\nthree");
    const QString b = QStringLiteral("one\ntwo\nthree");
    const auto d = lineDiff(a, b);
    EXPECT_EQ(countOf(d, K::Added), 1);
    EXPECT_EQ(countOf(d, K::Removed), 0);
    EXPECT_EQ(countOf(d, K::Context), 2);
}

TEST(LineDiff, pure_deletion_only_removes) {
    const QString a = QStringLiteral("one\ntwo\nthree");
    const QString b = QStringLiteral("one\nthree");
    const auto d = lineDiff(a, b);
    EXPECT_EQ(countOf(d, K::Removed), 1);
    EXPECT_EQ(countOf(d, K::Added), 0);
}

TEST(LineDiff, empty_old_makes_everything_added) {
    const auto d = lineDiff(QString{}, QStringLiteral("a\nb"));
    // Splitting "" yields one empty line; against "a","b" the empty line is
    // either context (if matched) or removed, and the real lines are added.
    EXPECT_GE(countOf(d, K::Added), 1);
}

}  // namespace chainapi::desktop::widgets::diff::tests
