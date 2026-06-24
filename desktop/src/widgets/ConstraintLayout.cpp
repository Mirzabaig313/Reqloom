// ConstraintLayout — see header. Distributes a 1-D length across segments
// honouring min/preferred/max + stretch weights.
#include "ConstraintLayout.h"

#include <algorithm>
#include <cmath>

namespace reqloom::desktop::layout {

namespace {

constexpr double kEpsilon = 1e-6;

[[nodiscard]] double clampSize(double value, double lo, double hi) {
    return std::max(lo, std::min(value, hi));
}

/// Grow segments by `surplus` total, weighted by stretch, capped at maximum.
/// Iterates so that length freed by a capped segment is re-shared among the
/// rest — converges in at most one pass per segment.
void grow(std::vector<double>& sizes, const std::vector<Constraint>& cons, double surplus) {
    const std::size_t n = sizes.size();
    for (std::size_t guard = 0; guard <= n && surplus > kEpsilon; ++guard) {
        double totalStretch = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            if (cons[i].stretch > 0.0 && sizes[i] < cons[i].maximum - kEpsilon) {
                totalStretch += cons[i].stretch;
            }
        }
        if (totalStretch <= 0.0) {
            break;
        }
        double remaining = surplus;
        bool capped = false;
        for (std::size_t i = 0; i < n; ++i) {
            if (cons[i].stretch <= 0.0 || sizes[i] >= cons[i].maximum - kEpsilon) {
                continue;
            }
            const double share = surplus * (cons[i].stretch / totalStretch);
            const double room = cons[i].maximum - sizes[i];
            const double applied = std::min(share, room);
            sizes[i] += applied;
            remaining -= applied;
            if (applied < share - kEpsilon) {
                capped = true;
            }
        }
        surplus = remaining;
        if (!capped) {
            break;
        }
    }
}

/// Shrink segments by `deficit` total, weighted by stretch where any stretch
/// exists among the shrinkable set, otherwise by the room above their minima.
/// Capped at minimum; iterates to converge.
void shrink(std::vector<double>& sizes, const std::vector<Constraint>& cons, double deficit) {
    const std::size_t n = sizes.size();
    for (std::size_t guard = 0; guard <= n && deficit > kEpsilon; ++guard) {
        double totalStretchWeight = 0.0;
        double totalRoomWeight = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            if (sizes[i] > cons[i].minimum + kEpsilon) {
                totalStretchWeight += cons[i].stretch;
                totalRoomWeight += sizes[i] - cons[i].minimum;
            }
        }
        const bool useStretch = totalStretchWeight > 0.0;
        const double totalWeight = useStretch ? totalStretchWeight : totalRoomWeight;
        if (totalWeight <= 0.0) {
            break;
        }
        double remaining = deficit;
        bool capped = false;
        for (std::size_t i = 0; i < n; ++i) {
            if (sizes[i] <= cons[i].minimum + kEpsilon) {
                continue;
            }
            const double weight = useStretch ? cons[i].stretch : (sizes[i] - cons[i].minimum);
            if (weight <= 0.0) {
                continue;
            }
            const double share = deficit * (weight / totalWeight);
            const double room = sizes[i] - cons[i].minimum;
            const double applied = std::min(share, room);
            sizes[i] -= applied;
            remaining -= applied;
            if (applied < share - kEpsilon) {
                capped = true;
            }
        }
        deficit = remaining;
        if (!capped) {
            break;
        }
    }
}

}  // namespace

std::vector<Placement> solveLinear(const std::vector<Constraint>& constraints,
                                   double available,
                                   double spacing) {
    const std::size_t n = constraints.size();
    if (n == 0) {
        return {};
    }

    std::vector<double> sizes(n, 0.0);
    double total = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        sizes[i] =
            clampSize(constraints[i].preferred, constraints[i].minimum, constraints[i].maximum);
        total += sizes[i];
    }

    const double gaps = spacing * static_cast<double>(n - 1);
    const double content = std::max(0.0, available - gaps);
    const double diff = content - total;
    if (diff > kEpsilon) {
        grow(sizes, constraints, diff);
    } else if (diff < -kEpsilon) {
        shrink(sizes, constraints, -diff);
    }

    std::vector<Placement> out(n);
    double cursor = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        out[i].offset = cursor;
        out[i].size = sizes[i];
        cursor += sizes[i] + spacing;
    }
    return out;
}

}  // namespace reqloom::desktop::layout
