// Unit tests for topologicalSort (Kahn's algorithm).
//
// Topology.h exposes a single free function; these tests exercise it in
// isolation so DependencyResolver tests can assume the sort is correct.
//
// Each test fails on the parent commit if the sort implementation is
// broken or the cycle-detection path is missing.
#include "domain/Topology.h"

#include <chainapi/engine/ErrorCodes.h>

#include <gtest/gtest.h>

namespace ce = chainapi::engine;

namespace {

using Graph = std::map<ce::OperationId, std::vector<ce::OperationId>>;

ce::OperationId op(std::string v) {
    return ce::OperationId{std::move(v)};
}

}  // namespace

// ─── Empty / trivial ────────────────────────────────────────────────────────

TEST(TopologicalSort, empty_graph_returns_empty_order) {
    auto result = ce::topologicalSort({});
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(result->empty());
}

TEST(TopologicalSort, single_node_no_edges_returns_that_node) {
    Graph g;
    g[op("a.get")] = {};

    auto result = ce::topologicalSort(g);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_EQ(result->size(), 1u);
    EXPECT_EQ((*result)[0].value, "a.get");
}

// ─── Linear chains ──────────────────────────────────────────────────────────

TEST(TopologicalSort, linear_chain_a_depends_on_b_depends_on_c) {
    // c → b → a  (c has no deps, a depends on b, b depends on c)
    Graph g;
    g[op("a.get")] = {op("b.get")};
    g[op("b.get")] = {op("c.get")};
    g[op("c.get")] = {};

    auto result = ce::topologicalSort(g);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_EQ(result->size(), 3u);

    // c must come before b, b must come before a.
    auto pos = [&](const std::string& v) -> std::size_t {
        for (std::size_t i = 0; i < result->size(); ++i) {
            if ((*result)[i].value == v) return i;
        }
        return std::string::npos;
    };
    EXPECT_LT(pos("c.get"), pos("b.get"));
    EXPECT_LT(pos("b.get"), pos("a.get"));
}

// ─── Diamond dependency ──────────────────────────────────────────────────────

TEST(TopologicalSort, diamond_dependency_is_resolved_without_duplicates) {
    // a depends on b and c; both b and c depend on d.
    //   d → b → a
    //   d → c → a
    Graph g;
    g[op("a.run")] = {op("b.run"), op("c.run")};
    g[op("b.run")] = {op("d.run")};
    g[op("c.run")] = {op("d.run")};
    g[op("d.run")] = {};

    auto result = ce::topologicalSort(g);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_EQ(result->size(), 4u);

    auto pos = [&](const std::string& v) -> std::size_t {
        for (std::size_t i = 0; i < result->size(); ++i) {
            if ((*result)[i].value == v) return i;
        }
        return std::string::npos;
    };
    EXPECT_LT(pos("d.run"), pos("b.run"));
    EXPECT_LT(pos("d.run"), pos("c.run"));
    EXPECT_LT(pos("b.run"), pos("a.run"));
    EXPECT_LT(pos("c.run"), pos("a.run"));
}

// ─── Lexicographic tie-break ─────────────────────────────────────────────────

TEST(TopologicalSort, independent_nodes_are_ordered_lexicographically) {
    // Three independent nodes — no edges between them.
    Graph g;
    g[op("z.op")] = {};
    g[op("a.op")] = {};
    g[op("m.op")] = {};

    auto result = ce::topologicalSort(g);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_EQ(result->size(), 3u);

    EXPECT_EQ((*result)[0].value, "a.op");
    EXPECT_EQ((*result)[1].value, "m.op");
    EXPECT_EQ((*result)[2].value, "z.op");
}

// ─── Cycle detection ─────────────────────────────────────────────────────────

TEST(TopologicalSort, direct_self_loop_returns_cycle_error) {
    Graph g;
    g[op("a.get")] = {op("a.get")};

    auto result = ce::topologicalSort(g);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::Cycle);
    EXPECT_EQ(result.error().cls, ce::ErrorClass::Schema);
}

TEST(TopologicalSort, two_node_cycle_returns_cycle_error) {
    Graph g;
    g[op("a.get")] = {op("b.get")};
    g[op("b.get")] = {op("a.get")};

    auto result = ce::topologicalSort(g);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::Cycle);
}

TEST(TopologicalSort, three_node_cycle_returns_cycle_error) {
    Graph g;
    g[op("a.get")] = {op("b.get")};
    g[op("b.get")] = {op("c.get")};
    g[op("c.get")] = {op("a.get")};

    auto result = ce::topologicalSort(g);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::Cycle);
}

TEST(TopologicalSort, cycle_in_subgraph_with_acyclic_prefix_returns_cycle_error) {
    // d → e → f → e  (d is acyclic; e-f form a cycle)
    Graph g;
    g[op("d.op")] = {op("e.op")};
    g[op("e.op")] = {op("f.op")};
    g[op("f.op")] = {op("e.op")};

    auto result = ce::topologicalSort(g);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ce::ErrorCode::Cycle);
}

// ─── Target is always last ────────────────────────────────────────────────────

TEST(TopologicalSort, target_node_with_deps_appears_last) {
    // Simulates the DependencyResolver contract: the target op is the
    // last element in the sorted chain.
    Graph g;
    g[op("refund.approve")] = {op("refund.request")};
    g[op("refund.request")] = {op("order.pay")};
    g[op("order.pay")] = {op("order.create")};
    g[op("order.create")] = {};

    auto result = ce::topologicalSort(g);
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_EQ(result->size(), 4u);
    EXPECT_EQ(result->back().value, "refund.approve");
}
