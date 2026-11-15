// ConstraintLayout — a small linear (1-D) constraint solver for a row of segments. Each declares a
// minimum, preferred size, maximum and stretch weight instead of pixel numbers (e.g. "method combo
// = content width, path field fills the rest"). Pure logic; the QML LayoutSolver exposes it.
#pragma once

#include <limits>
#include <vector>

namespace reqloom::desktop::layout {

/// One element to place along the axis. `stretch` 0 means "stay at preferred"
/// (a fixed/intrinsic element); a positive weight means "absorb spare length in
/// proportion to this weight" (a filling element). `maximum` caps growth.
struct Constraint {
    double minimum{0.0};
    double preferred{0.0};
    double maximum{std::numeric_limits<double>::max()};
    double stretch{0.0};
};

/// Resolved placement of one segment (parallel to the input order).
struct Placement {
    double offset{0.0};  ///< Start position along the axis.
    double size{0.0};    ///< Resolved length.
};

/// Solve `constraints` into placements spanning `available` length, separated by `spacing`.
/// Segments start at preferred size (clamped); spare length goes to stretchable segments by weight
/// (respecting maxima), a deficit shrinks toward minima (stretchable first). Deterministic.
[[nodiscard]] std::vector<Placement> solveLinear(const std::vector<Constraint>& constraints,
                                                 double available,
                                                 double spacing = 0.0);

}  // namespace reqloom::desktop::layout
