// Resolved dependency plan: a target's execution chain with explicit `depends_on` and implicit
// `{{resource.var}}` edges.
#pragma once

#include <reqloom/engine/Operation.h>

#include <string>
#include <vector>

namespace reqloom::engine {

/// One resolved dependency: `consumer` runs after `producer`. `implicit` marks an edge derived from
/// a `{{resource.var}}` reference; `variable` then names the flowing value (e.g.
/// "product.product_id").
struct DependencyEdge {
    OperationId consumer;
    OperationId producer;
    bool implicit{false};
    std::string variable{};
};

/// A target operation's resolved chain: the topological execution `order`
/// (dependencies first, target last) plus the `edges` that produced it.
struct ResolvedPlan {
    std::vector<OperationId> order;
    std::vector<DependencyEdge> edges;
};

}  // namespace reqloom::engine
