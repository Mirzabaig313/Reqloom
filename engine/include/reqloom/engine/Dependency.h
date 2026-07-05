// Resolved dependency plan — the engine's view of a target's execution chain,
// including edges derived from both explicit `depends_on` and implicit
// `{{resource.var}}` usage. Lets embedders (the desktop graph) draw the *real*
// resolved chain instead of re-deriving only the declared edges.
#pragma once

#include <reqloom/engine/Operation.h>

#include <string>
#include <vector>

namespace reqloom::engine {

/// One resolved dependency: `consumer` runs after `producer`. `implicit` is
/// true when the edge was derived from a `{{resource.var}}` reference rather
/// than a declared `depends_on`; `variable` then names the flowing value
/// (e.g. "product.product_id"). For explicit edges `variable` is empty.
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
