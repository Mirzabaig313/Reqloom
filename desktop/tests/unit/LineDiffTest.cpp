// Tests the LCS line differ that backs the response Diff tab (FR-7.5).
#include "widgets/LineDiff.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <random>
#include <vector>

namespace reqloom::desktop::widgets::diff::tests {

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

[[nodiscard]] QString numberedLines(int count, const QString& prefix) {
    QStringList lines;
    lines.reserve(count);
    for (int i = 0; i < count; ++i) {
        lines.append(prefix + QString::number(i));
    }
    return lines.join(QLatin1Char('\n'));
}

[[nodiscard]] long long elapsedMs(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 since)
        .count();
}

// Classic O(n·m) LCS length, the reference a minimal diff must match on small inputs.
[[nodiscard]] int lcsLength(const QStringList& a, const QStringList& b) {
    std::vector<std::vector<int>> t(static_cast<std::size_t>(a.size()) + 1,
                                    std::vector<int>(static_cast<std::size_t>(b.size()) + 1, 0));
    for (qsizetype i = a.size() - 1; i >= 0; --i) {
        for (qsizetype j = b.size() - 1; j >= 0; --j) {
            const auto ui = static_cast<std::size_t>(i);
            const auto uj = static_cast<std::size_t>(j);
            t[ui][uj] =
                a[i] == b[j] ? t[ui + 1][uj + 1] + 1 : std::max(t[ui + 1][uj], t[ui][uj + 1]);
        }
    }
    return t[0][0];
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

TEST(LineDiff, random_inputs_rebuild_both_texts_with_a_minimal_diff) {
    std::mt19937 rng{1234};
    std::uniform_int_distribution<int> lenDist{0, 12};
    std::uniform_int_distribution<int> symDist{0, 3};
    for (int round = 0; round < 500; ++round) {
        QStringList a;
        QStringList b;
        for (int i = lenDist(rng); i > 0; --i) {
            a.append(QString::number(symDist(rng)));
        }
        for (int i = lenDist(rng); i > 0; --i) {
            b.append(QString::number(symDist(rng)));
        }
        const auto d = lineDiff(a.join(QLatin1Char('\n')), b.join(QLatin1Char('\n')));

        QStringList rebuiltOld;
        QStringList rebuiltNew;
        for (const auto& line : d) {
            if (line.kind != K::Added) {
                rebuiltOld.append(line.text);
            }
            if (line.kind != K::Removed) {
                rebuiltNew.append(line.text);
            }
        }
        const QStringList splitA = a.join(QLatin1Char('\n')).split(QLatin1Char('\n'));
        const QStringList splitB = b.join(QLatin1Char('\n')).split(QLatin1Char('\n'));
        ASSERT_EQ(rebuiltOld, splitA);
        ASSERT_EQ(rebuiltNew, splitB);
        ASSERT_EQ(countOf(d, K::Context), lcsLength(splitA, splitB));
    }
}

TEST(LineDiff, removals_precede_additions_within_a_change) {
    const auto d = lineDiff(QStringLiteral("a\nx\ny\nb"), QStringLiteral("a\nP\nQ\nR\nb"));
    bool seenAdded = false;
    for (const auto& line : d) {
        if (line.kind == K::Added) {
            seenAdded = true;
        } else if (line.kind == K::Removed) {
            EXPECT_FALSE(seenAdded);
        } else {
            seenAdded = false;
        }
    }
}

// 16,000 lines a side would need a 1 GB table in a full LCS matrix.
TEST(LineDiff, large_similar_bodies_diff_quickly) {
    QStringList lines = numberedLines(16000, QStringLiteral("line ")).split(QLatin1Char('\n'));
    const QString oldText = lines.join(QLatin1Char('\n'));
    lines[100] = QStringLiteral("changed 100");
    lines[8000] = QStringLiteral("changed 8000");
    lines.insert(15000, QStringLiteral("inserted"));
    const QString newText = lines.join(QLatin1Char('\n'));
    const auto started = std::chrono::steady_clock::now();

    const auto result = boundedLineDiff(oldText, newText);

    EXPECT_LT(elapsedMs(started), 2000);
    ASSERT_FALSE(result.tooLarge);
    EXPECT_EQ(countOf(result.lines, K::Removed), 2);
    EXPECT_EQ(countOf(result.lines, K::Added), 3);
    EXPECT_EQ(countOf(result.lines, K::Context), 15998);
}

TEST(LineDiff, unchanged_prefix_and_suffix_do_not_count_toward_the_size_limit) {
    const QString common = numberedLines(30000, QStringLiteral("same "));
    const QString oldText = common + QStringLiteral("\nold\n") + common;
    const QString newText = common + QStringLiteral("\nnew\n") + common;

    const auto result = boundedLineDiff(oldText, newText);

    ASSERT_FALSE(result.tooLarge);
    EXPECT_EQ(countOf(result.lines, K::Removed), 1);
    EXPECT_EQ(countOf(result.lines, K::Added), 1);
}

TEST(LineDiff, more_changed_lines_than_the_limit_report_too_large) {
    const int lines = kMaxDiffLines + 1;
    const auto started = std::chrono::steady_clock::now();

    const auto result = boundedLineDiff(numberedLines(lines, QStringLiteral("old ")),
                                        numberedLines(lines, QStringLiteral("new ")));

    EXPECT_LT(elapsedMs(started), 2000);
    EXPECT_TRUE(result.tooLarge);
    EXPECT_TRUE(result.lines.empty());
}

TEST(LineDiff, completely_different_bodies_within_the_line_limit_stop_at_the_work_budget) {
    const auto started = std::chrono::steady_clock::now();

    const auto result = boundedLineDiff(numberedLines(16000, QStringLiteral("old ")),
                                        numberedLines(16000, QStringLiteral("new ")));

    EXPECT_LT(elapsedMs(started), 3000);
    EXPECT_TRUE(result.tooLarge);
}

TEST(LineDiff, unbounded_entry_point_falls_back_to_replacing_everything_when_too_large) {
    const int lines = kMaxDiffLines + 1;

    const auto d = lineDiff(numberedLines(lines, QStringLiteral("old ")),
                            numberedLines(lines, QStringLiteral("new ")));

    EXPECT_EQ(countOf(d, K::Removed), lines);
    EXPECT_EQ(countOf(d, K::Added), lines);
}

}  // namespace reqloom::desktop::widgets::diff::tests
