// ConstraintLayout — a small linear (1-D) constraint solver for laying out a
// row (or column) of segments. Pure logic, no Qt dependency, so it's unit-
// tested in isolation. Each segment declares intent — a minimum, a preferred
// (intrinsic/content) size, a maximum, and a stretch weight — instead of magic
// pixel numbers; the solver distributes the available length to honour those
// constraints (e.g. "method combo = its content width, path field fills the
// remainder"). The QML LayoutSolver exposes this to the request bar.
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

/// Solve a strip of `constraints` into placements that span `available` length,
/// separated by `spacing`. Segments start at their preferred size (clamped to
/// [minimum, maximum]); spare length is shared among stretchable segments in
/// proportion to their weight (respecting maxima), and a deficit shrinks
/// segments down toward their minima (stretchable ones first). Deterministic.
[[nodiscard]] std::vector<Placement> solveLinear(const std::vector<Constraint>& constraints,
                                                 double available,
                                                 double spacing = 0.0);

}  // namespace reqloom::desktop::layout
