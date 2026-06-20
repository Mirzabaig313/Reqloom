// GraphLayout — a small layered (Sugiyama-style) DAG layout for the execution
// chain view. Pure logic, no Qt-widget dependency, so it's unit-tested in
// isolation. Given a node count + directed edges (prerequisite → dependent),
// it assigns each node to a layer (longest-path), orders nodes within layers to
// reduce edge crossings (barycenter sweeps), and produces top-left coordinates
// plus the overall canvas size. The QML ChainView renders nodes at these
// positions and draws Bézier edges between them.
#pragma once

#include <cstddef>
#include <utility>
#include <vector>

namespace reqloom::desktop::layout {

/// Resolved placement of one node (parallel to the input node indices).
struct LayoutNode {
    int layer{0};   ///< 0 = topmost (a source / prerequisite with no deps).
    int order{0};   ///< Position within the layer, left → right.
    double x{0.0};  ///< Top-left x in layout units.
    double y{0.0};  ///< Top-left y in layout units.
};

/// Tunable spacing. Defaults suit a ~220×34 node card.
struct LayoutOptions {
    double nodeWidth{220.0};
    double nodeHeight{34.0};
    double hGap{24.0};  ///< Horizontal gap between nodes in a layer.
    double vGap{40.0};  ///< Vertical gap between layers.
};

/// Full layout result: per-node placement + the bounding canvas size.
struct LayoutResult {
    std::vector<LayoutNode> nodes;
    double width{0.0};
    double height{0.0};
};

/// Lay out a DAG of `nodeCount` nodes (indices 0..nodeCount-1) connected by
/// directed `edges` (first = prerequisite, second = dependent). Prerequisites
/// sit above their dependents. Self-loops and out-of-range indices are ignored;
/// cycles are tolerated (nodes on a cycle simply settle at an early layer).
[[nodiscard]] LayoutResult layeredLayout(int nodeCount,
                                         const std::vector<std::pair<int, int>>& edges,
                                         const LayoutOptions& options = {});

}  // namespace reqloom::desktop::layout
