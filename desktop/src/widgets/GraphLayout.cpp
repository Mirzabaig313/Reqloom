// GraphLayout — see header. Layered DAG layout: longest-path layering →
// barycenter crossing reduction → centered coordinate assignment.
#include "GraphLayout.h"

#include <algorithm>
#include <numeric>
#include <queue>

namespace reqloom::desktop::layout {

namespace {

// Longest-path layering via Kahn's topological order: O(V+E), no recursion.
// layer[v] = max over incoming edges (u→v) of layer[u] + 1. Nodes left on a
// cycle never reach in-degree 0, so they keep their initial layer 0 — a benign
// fallback that still draws something rather than looping forever.
[[nodiscard]] std::vector<int> assignLayers(int n,
                                            const std::vector<std::vector<int>>& out,
                                            std::vector<int> indegree) {
    std::vector<int> layer(static_cast<std::size_t>(n), 0);
    std::queue<int> ready;
    for (int v = 0; v < n; ++v) {
        if (indegree[static_cast<std::size_t>(v)] == 0) {
            ready.push(v);
        }
    }
    while (!ready.empty()) {
        const int u = ready.front();
        ready.pop();
        for (const int v : out[static_cast<std::size_t>(u)]) {
            auto& lv = layer[static_cast<std::size_t>(v)];
            lv = std::max(lv, layer[static_cast<std::size_t>(u)] + 1);
            if (--indegree[static_cast<std::size_t>(v)] == 0) {
                ready.push(v);
            }
        }
    }
    return layer;
}

// One ordering sweep over `target` using neighbours in the `fixed` layer: each
// node's barycenter is the mean order-position of its neighbours; nodes are
// then re-sorted by that barycenter. Nodes with no neighbour keep their slot.
void barycenterSweep(std::vector<int>& target,
                     const std::vector<int>& order,
                     const std::vector<std::vector<int>>& neighbours) {
    if (target.size() < 2) {
        return;
    }
    std::stable_sort(target.begin(), target.end(), [&](int a, int b) {
        const auto bary = [&](int node) -> double {
            const auto& ns = neighbours[static_cast<std::size_t>(node)];
            if (ns.empty()) {
                return static_cast<double>(order[static_cast<std::size_t>(node)]);
            }
            double sum = 0.0;
            for (const int m : ns) {
                sum += order[static_cast<std::size_t>(m)];
            }
            return sum / static_cast<double>(ns.size());
        };
        return bary(a) < bary(b);
    });
}

}  // namespace

LayoutResult layeredLayout(int nodeCount,
                           const std::vector<std::pair<int, int>>& edges,
                           const LayoutOptions& options) {
    LayoutResult result;
    if (nodeCount <= 0) {
        return result;
    }
    const auto n = static_cast<std::size_t>(nodeCount);
    result.nodes.assign(n, LayoutNode{});

    std::vector<std::vector<int>> out(n);
    std::vector<std::vector<int>> in(n);
    std::vector<int> indegree(n, 0);
    for (const auto& [from, to] : edges) {
        if (from == to || from < 0 || to < 0 || from >= nodeCount || to >= nodeCount) {
            continue;
        }
        out[static_cast<std::size_t>(from)].push_back(to);
        in[static_cast<std::size_t>(to)].push_back(from);
        ++indegree[static_cast<std::size_t>(to)];
    }

    const std::vector<int> layer = assignLayers(nodeCount, out, indegree);
    int layerCount = 0;
    for (const int l : layer) {
        layerCount = std::max(layerCount, l + 1);
    }

    // Group node indices by layer, seeded in index order.
    std::vector<std::vector<int>> byLayer(static_cast<std::size_t>(layerCount));
    for (int v = 0; v < nodeCount; ++v) {
        byLayer[static_cast<std::size_t>(layer[static_cast<std::size_t>(v)])].push_back(v);
    }

    // order[v] = current position of v within its layer.
    std::vector<int> order(n, 0);
    const auto reindex = [&]() {
        for (const auto& nodes : byLayer) {
            for (int pos = 0; pos < static_cast<int>(nodes.size()); ++pos) {
                order[static_cast<std::size_t>(nodes[static_cast<std::size_t>(pos)])] = pos;
            }
        }
    };
    reindex();

    // Crossing reduction: alternate downward (order by parents) and upward
    // (order by children) barycenter sweeps. A handful of passes converges for
    // the small graphs a dependency chain produces.
    constexpr int kSweeps = 6;
    for (int sweep = 0; sweep < kSweeps; ++sweep) {
        const bool downward = (sweep % 2) == 0;
        if (downward) {
            for (int l = 1; l < layerCount; ++l) {
                barycenterSweep(byLayer[static_cast<std::size_t>(l)], order, in);
                reindex();
            }
        } else {
            for (int l = layerCount - 2; l >= 0; --l) {
                barycenterSweep(byLayer[static_cast<std::size_t>(l)], order, out);
                reindex();
            }
        }
    }

    // Coordinate assignment: each layer is centred within the widest layer.
    double canvasWidth = 0.0;
    for (const auto& nodes : byLayer) {
        const double w = nodes.empty() ? 0.0
                                       : (static_cast<double>(nodes.size()) * options.nodeWidth) +
                                             (static_cast<double>(nodes.size() - 1) * options.hGap);
        canvasWidth = std::max(canvasWidth, w);
    }

    for (int l = 0; l < layerCount; ++l) {
        const auto& nodes = byLayer[static_cast<std::size_t>(l)];
        const double layerWidth = nodes.empty()
                                      ? 0.0
                                      : (static_cast<double>(nodes.size()) * options.nodeWidth) +
                                            (static_cast<double>(nodes.size() - 1) * options.hGap);
        const double startX = (canvasWidth - layerWidth) / 2.0;
        const double y = static_cast<double>(l) * (options.nodeHeight + options.vGap);
        for (int pos = 0; pos < static_cast<int>(nodes.size()); ++pos) {
            const auto v = static_cast<std::size_t>(nodes[static_cast<std::size_t>(pos)]);
            result.nodes[v].layer = l;
            result.nodes[v].order = pos;
            result.nodes[v].x =
                startX + (static_cast<double>(pos) * (options.nodeWidth + options.hGap));
            result.nodes[v].y = y;
        }
    }

    result.width = canvasWidth;
    result.height = layerCount > 0 ? (static_cast<double>(layerCount) * options.nodeHeight) +
                                         (static_cast<double>(layerCount - 1) * options.vGap)
                                   : 0.0;

    // Edge-straightening pass: pull each node toward the mean x of its
    // neighbours (parents + children), then resolve same-layer overlaps
    // left-to-right. A per-iteration snapshot (Jacobi update) keeps symmetric
    // structures settling symmetrically rather than drifting with order.
    if (layerCount > 1) {
        constexpr int kAlignIterations = 6;
        for (int iter = 0; iter < kAlignIterations; ++iter) {
            std::vector<double> snapshot(n);
            for (std::size_t v = 0; v < n; ++v) {
                snapshot[v] = result.nodes[v].x;
            }
            for (int l = 0; l < layerCount; ++l) {
                const auto& row = byLayer[static_cast<std::size_t>(l)];
                for (const int v : row) {
                    double sum = 0.0;
                    int count = 0;
                    for (const int u : in[static_cast<std::size_t>(v)]) {
                        sum += snapshot[static_cast<std::size_t>(u)];
                        ++count;
                    }
                    for (const int w : out[static_cast<std::size_t>(v)]) {
                        sum += snapshot[static_cast<std::size_t>(w)];
                        ++count;
                    }
                    if (count > 0) {
                        result.nodes[static_cast<std::size_t>(v)].x =
                            sum / static_cast<double>(count);
                    }
                }
                for (std::size_t p = 1; p < row.size(); ++p) {
                    const auto prev = static_cast<std::size_t>(row[p - 1]);
                    const auto cur = static_cast<std::size_t>(row[p]);
                    const double minX = result.nodes[prev].x + options.nodeWidth + options.hGap;
                    if (result.nodes[cur].x < minX) {
                        result.nodes[cur].x = minX;
                    }
                }
            }
        }
        // Normalise to a non-negative origin; recompute the canvas width.
        double minX = result.nodes[0].x;
        double maxX = result.nodes[0].x + options.nodeWidth;
        for (const auto& node : result.nodes) {
            minX = std::min(minX, node.x);
            maxX = std::max(maxX, node.x + options.nodeWidth);
        }
        for (auto& node : result.nodes) {
            node.x -= minX;
        }
        result.width = maxX - minX;
    }

    return result;
}

}  // namespace reqloom::desktop::layout
