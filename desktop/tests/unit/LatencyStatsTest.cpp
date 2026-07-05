// Tests latency summary statistics + Freedman–Diaconis histogram binning.
// Pure logic — no Qt.
#include "widgets/LatencyStats.h"

#include <gtest/gtest.h>

#include <numeric>
#include <vector>

namespace reqloom::desktop::stats::tests {

TEST(LatencyStats, empty_input_is_zeroed) {
    const Summary s = summarize({});
    EXPECT_EQ(s.count, 0u);
    EXPECT_DOUBLE_EQ(s.median, 0.0);
    EXPECT_TRUE(histogram({}).counts.empty());
}

TEST(LatencyStats, single_sample_reports_that_value) {
    const Summary s = summarize({42.0});
    EXPECT_EQ(s.count, 1u);
    EXPECT_DOUBLE_EQ(s.min, 42.0);
    EXPECT_DOUBLE_EQ(s.max, 42.0);
    EXPECT_DOUBLE_EQ(s.mean, 42.0);
    EXPECT_DOUBLE_EQ(s.median, 42.0);
    EXPECT_DOUBLE_EQ(s.p95, 42.0);
}

TEST(LatencyStats, summary_is_order_independent) {
    const Summary a = summarize({10.0, 20.0, 30.0, 40.0, 50.0});
    const Summary b = summarize({30.0, 10.0, 50.0, 20.0, 40.0});
    EXPECT_DOUBLE_EQ(a.median, b.median);
    EXPECT_DOUBLE_EQ(a.median, 30.0);
    EXPECT_DOUBLE_EQ(a.mean, 30.0);
    EXPECT_DOUBLE_EQ(a.min, 10.0);
    EXPECT_DOUBLE_EQ(a.max, 50.0);
}

TEST(LatencyStats, percentile_interpolates_between_ranks) {
    // Type-7: p95 of 0..100 (101 values) is exactly 95.
    std::vector<double> v(101);
    std::iota(v.begin(), v.end(), 0.0);
    EXPECT_DOUBLE_EQ(percentileSorted(v, 0.95), 95.0);
    EXPECT_DOUBLE_EQ(percentileSorted(v, 0.5), 50.0);
}

TEST(LatencyStats, identical_values_yield_one_bin) {
    const Histogram h = histogram({7.0, 7.0, 7.0});
    ASSERT_EQ(h.counts.size(), 1u);
    EXPECT_EQ(h.counts[0], 3);
    EXPECT_DOUBLE_EQ(h.start, 7.0);
}

TEST(LatencyStats, histogram_counts_every_sample) {
    const std::vector<double> v{5, 10, 12, 15, 18, 20, 22, 25, 30, 100};
    const Histogram h = histogram(v);
    ASSERT_FALSE(h.counts.empty());
    int total = 0;
    for (const int c : h.counts) {
        total += c;
    }
    EXPECT_EQ(total, static_cast<int>(v.size()));
    EXPECT_GT(h.binWidth, 0.0);
    EXPECT_DOUBLE_EQ(h.start, 5.0);
}

TEST(LatencyStats, max_value_lands_in_last_bin) {
    const std::vector<double> v{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    const Histogram h = histogram(v);
    ASSERT_FALSE(h.counts.empty());
    EXPECT_GE(h.counts.back(), 1);  // the value 10 is not dropped past the edge
}

}  // namespace reqloom::desktop::stats::tests
