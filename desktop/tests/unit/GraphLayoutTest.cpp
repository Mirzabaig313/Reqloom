// Tests the layered DAG layout: layering (longest path), within-layer ordering,
// coordinate assignment, and resilience to cycles. Pure logic — no Qt widgets.
#include "widgets/GraphLayout.h"

#include <gtest/gtest.h>

namespace reqloom::desktop::layout::tests {

TEST(GraphLayout, empty_graph_is_empty) {
    const LayoutResult r = layeredLayout(0, {});
    EXPECT_TRUE(r.nodes.empty());
    EXPECT_DOUBLE_EQ(r.width, 0.0);
    EXPECT_DOUBLE_EQ(r.height, 0.0);
}

TEST(GraphLayout, single_node_sits_at_origin_layer) {
    const LayoutResult r = layeredLayout(1, {});
    ASSERT_EQ(r.nodes.size(), 1u);
    EXPECT_EQ(r.nodes[0].layer, 0);
    EXPECT_DOUBLE_EQ(r.nodes[0].x, 0.0);
    EXPECT_DOUBLE_EQ(r.nodes[0].y, 0.0);
}

TEST(GraphLayout, linear_chain_stacks_one_per_layer) {
    // 0 → 1 → 2 : each is a prerequisite of the next, so layers are 0,1,2.
    const LayoutResult r = layeredLayout(3, {{0, 1}, {1, 2}});
    ASSERT_EQ(r.nodes.size(), 3u);
    EXPECT_EQ(r.nodes[0].layer, 0);
    EXPECT_EQ(r.nodes[1].layer, 1);
    EXPECT_EQ(r.nodes[2].layer, 2);
    // Strictly increasing y down the chain.
    EXPECT_LT(r.nodes[0].y, r.nodes[1].y);
    EXPECT_LT(r.nodes[1].y, r.nodes[2].y);
    // After straightening, a single-file chain is vertically aligned (same x).
    EXPECT_DOUBLE_EQ(r.nodes[0].x, r.nodes[1].x);
    EXPECT_DOUBLE_EQ(r.nodes[1].x, r.nodes[2].x);
}

TEST(GraphLayout, diamond_places_two_middles_on_one_layer) {
    // 0 → 1, 0 → 2, 1 → 3, 2 → 3 (a diamond). Layers: 0=0, {1,2}=1, 3=2.
    const LayoutResult r = layeredLayout(4, {{0, 1}, {0, 2}, {1, 3}, {2, 3}});
    ASSERT_EQ(r.nodes.size(), 4u);
    EXPECT_EQ(r.nodes[0].layer, 0);
    EXPECT_EQ(r.nodes[1].layer, 1);
    EXPECT_EQ(r.nodes[2].layer, 1);
    EXPECT_EQ(r.nodes[3].layer, 2);
    // The two middle nodes share a layer (same y) but sit at different x.
    EXPECT_DOUBLE_EQ(r.nodes[1].y, r.nodes[2].y);
    EXPECT_NE(r.nodes[1].x, r.nodes[2].x);
    // Root and sink are centred over the wider middle layer (same centre x).
    const double rootCx = r.nodes[0].x;
    const double sinkCx = r.nodes[3].x;
    EXPECT_DOUBLE_EQ(rootCx, sinkCx);
}

TEST(GraphLayout, longest_path_wins_when_a_node_has_two_depths) {
    // 0 → 2 directly, and 0 → 1 → 2. Node 2's layer is the longest path = 2.
    const LayoutResult r = layeredLayout(3, {{0, 1}, {1, 2}, {0, 2}});
    EXPECT_EQ(r.nodes[2].layer, 2);
}

TEST(GraphLayout, cycle_is_tolerated_without_hanging) {
    // A 2-cycle plus a normal edge must still return a finite result.
    const LayoutResult r = layeredLayout(3, {{0, 1}, {1, 0}, {1, 2}});
    EXPECT_EQ(r.nodes.size(), 3u);
    EXPECT_GE(r.height, 0.0);
}

TEST(GraphLayout, self_loop_and_bad_indices_are_ignored) {
    const LayoutResult r = layeredLayout(2, {{0, 0}, {-1, 1}, {0, 5}, {0, 1}});
    ASSERT_EQ(r.nodes.size(), 2u);
    EXPECT_EQ(r.nodes[0].layer, 0);
    EXPECT_EQ(r.nodes[1].layer, 1);
}

}  // namespace reqloom::desktop::layout::tests
