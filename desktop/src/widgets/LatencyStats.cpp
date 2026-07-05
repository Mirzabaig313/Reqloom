// LatencyStats — see header. Pure summary + Freedman–Diaconis binning.
#include "LatencyStats.h"

#include <algorithm>
#include <cmath>

namespace reqloom::desktop::stats {

double percentileSorted(const std::vector<double>& sorted, double q) {
    if (sorted.empty()) {
        return 0.0;
    }
    if (sorted.size() == 1) {
        return sorted.front();
    }
    const double clampedQ = std::clamp(q, 0.0, 1.0);
    // Type-7 (linear interpolation between order statistics), as used by
    // NumPy/R defaults: rank = q * (n - 1).
    const double rank = clampedQ * static_cast<double>(sorted.size() - 1);
    const auto lo = static_cast<std::size_t>(std::floor(rank));
    const auto hi = static_cast<std::size_t>(std::ceil(rank));
    const double frac = rank - static_cast<double>(lo);
    return sorted[lo] + ((sorted[hi] - sorted[lo]) * frac);
}

Summary summarize(std::vector<double> samples) {
    Summary out;
    if (samples.empty()) {
        return out;
    }
    std::sort(samples.begin(), samples.end());
    out.count = samples.size();
    out.min = samples.front();
    out.max = samples.back();
    double sum = 0.0;
    for (const double v : samples) {
        sum += v;
    }
    out.mean = sum / static_cast<double>(samples.size());
    out.median = percentileSorted(samples, 0.5);
    out.p95 = percentileSorted(samples, 0.95);
    out.p99 = percentileSorted(samples, 0.99);
    return out;
}

Histogram histogram(std::vector<double> samples) {
    Histogram out;
    if (samples.empty()) {
        return out;
    }
    std::sort(samples.begin(), samples.end());
    const std::size_t n = samples.size();
    const double lo = samples.front();
    const double hi = samples.back();
    const double range = hi - lo;

    // All identical → a single unit-width bin holding everything.
    if (range <= 0.0) {
        out.start = lo;
        out.binWidth = 1.0;
        out.counts = {static_cast<int>(n)};
        return out;
    }

    // Freedman–Diaconis bin width; fall back to √n bins when it degenerates.
    const double iqr = percentileSorted(samples, 0.75) - percentileSorted(samples, 0.25);
    double binWidth = 0.0;
    if (n >= 2 && iqr > 0.0) {
        binWidth = 2.0 * iqr * std::pow(static_cast<double>(n), -1.0 / 3.0);
    }
    int binCount = 0;
    if (binWidth > 0.0) {
        binCount = std::max(1, static_cast<int>(std::ceil(range / binWidth)));
    } else {
        binCount = std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n)))));
        binWidth = range / static_cast<double>(binCount);
    }

    out.start = lo;
    out.binWidth = binWidth;
    out.counts.assign(static_cast<std::size_t>(binCount), 0);
    for (const double v : samples) {
        auto bin = static_cast<int>((v - lo) / binWidth);
        bin = std::clamp(bin, 0, binCount - 1);  // the max value lands in the last bin
        ++out.counts[static_cast<std::size_t>(bin)];
    }
    return out;
}

}  // namespace reqloom::desktop::stats
