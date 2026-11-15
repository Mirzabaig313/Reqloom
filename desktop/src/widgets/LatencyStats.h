// LatencyStats — pure summary statistics and histogram binning for response latencies. Percentiles
// are type-7; the histogram uses Freedman–Diaconis (bin width = 2·IQR·n^(-1/3)) with a √n fallback
// when n < 2 or IQR is zero.
#pragma once

#include <cstddef>
#include <vector>

namespace reqloom::desktop::stats {

/// Most bins histogram() returns, so one slow outlier cannot produce thousands of bars.
inline constexpr int kMaxHistogramBins = 64;

/// Summary of a latency sample set (all values in the input unit, e.g. ms).
struct Summary {
    std::size_t count{0};
    double min{0.0};
    double max{0.0};
    double mean{0.0};
    double median{0.0};
    double p95{0.0};
    double p99{0.0};
};

/// A histogram: `counts[i]` samples fall in `[start + i*binWidth, start +
/// (i+1)*binWidth)`. Empty when there are no samples.
struct Histogram {
    double start{0.0};
    double binWidth{0.0};
    std::vector<int> counts;
};

/// Summary statistics of `samples` (order-independent; the input is copied and
/// sorted internally). An empty input yields a zeroed summary.
[[nodiscard]] Summary summarize(std::vector<double> samples);

/// Freedman–Diaconis histogram of `samples`. Falls back to ⌈√n⌉ bins when the
/// IQR is zero or n < 2, and widens the bins when more than kMaxHistogramBins
/// would be needed. A single distinct value yields one unit-width bin.
[[nodiscard]] Histogram histogram(std::vector<double> samples);

/// Linear-interpolated percentile (`q` in [0,1]) of an already-sorted,
/// non-empty range. Exposed for reuse + direct testing.
[[nodiscard]] double percentileSorted(const std::vector<double>& sorted, double q);

}  // namespace reqloom::desktop::stats
