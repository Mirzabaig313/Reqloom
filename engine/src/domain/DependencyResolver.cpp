// DependencyResolver — Engine Requirement §3.1.
//
// Builds the execution chain for a target operation using topological sort
// (Kahn's algorithm) with deterministic lexicographic tie-break (AC-3.1.3).
// Detects cycles (AC-3.1.4) and undefined references (AC-3.1.6).
#include "DependencyResolver.h"

#include <queue>
#include <regex>
#include <set>

namespace chainapi::engine {

namespace {

/// Extract all implicit dependencies from an operation's templates.
/// A reference like `{{product.product_id}}` implies a dependency on
/// the resource "product" — specifically on whatever operation produces
/// that variable (i.e. has it in `extract:`).
std::vector<OperationId> inferImplicitDeps(
    const Operation& op,
    const Project& project) {

    // Collect all template strings from the operation.
    std::vector<std::string> templates;
    templates.push_back(op.pathTemplate);
    if (op.bodyTemplate) templates.push_back(*op.bodyTemplate);
    for (const auto& [_, v] : op.headers) templates.push_back(v);
    for (const auto& [_, v] : op.queryParams) templates.push_back(v);
    if (op.bodyForm) {
        for (const auto& [_, v] : *op.bodyForm) templates.push_back(v);
    }

    // Find all {{X.y}} or {{X[N].y}} references.
    // The first capture group is the resource name (without index brackets);
    // optional `[N]` is consumed but not captured for dependency purposes.
    static const std::regex refPattern(R"(\{\{(\w+)(?:\[\d+\])?\.(\w+)\}\})");
    std::set<OperationId> deps;

    for (const auto& tmpl : templates) {
        auto begin = std::sregex_iterator(tmpl.begin(), tmpl.end(), refPattern);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            auto refResource = (*it)[1].str();
            auto refVar = (*it)[2].str();

            // Skip builtins ($.uuid, $.now, $.faker.*, $.env.*)
            if (refResource == "$") continue;
            // Skip env references
            if (refResource == "env" || refResource == "secret") continue;

            // Check if this is an actor reference (e.g. {{vendor.token}})
            bool isActor = false;
            for (const auto& [actorId, _] : project.actors) {
                if (actorId.value == refResource) {
                    isActor = true;
                    break;
                }
            }
            if (isActor) continue;  // Actor deps are handled by session system, not chain.

            // It's a resource reference. Find which operation produces this variable.
            auto resIt = project.resources.find(ResourceId{refResource});
            if (resIt == project.resources.end()) continue;  // Undefined; caught by validator.

            for (const auto& [opName, resOp] : resIt->second.operations) {
                for (const auto& ext : resOp.extractions) {
                    if (ext.variableName == refVar) {
                        auto depId = OperationId{refResource + "." + opName};
                        // Don't add self-dep
                        if (depId.value != op.id.value) {
                            deps.insert(depId);
                        }
                        break;
                    }
                }
            }
        }
    }

    return {deps.begin(), deps.end()};
}

}  // namespace

DependencyResolver::DependencyResolver() = default;
DependencyResolver::~DependencyResolver() = default;

std::expected<std::vector<OperationId>, ChainApiError>
DependencyResolver::resolve(const Project& project,
                            const OperationId& target) const {
    // 1. Build the full dependency graph (explicit + implicit edges) for the
    //    transitive closure of `target`.
    std::map<OperationId, std::vector<OperationId>> graph;  // node → its dependencies
    std::set<OperationId> visited;
    std::queue<OperationId> worklist;
    worklist.push(target);

    while (!worklist.empty()) {
        auto current = worklist.front();
        worklist.pop();

        if (visited.contains(current)) continue;
        visited.insert(current);

        // Find the Operation in the project.
        auto dotPos = current.value.find('.');
        if (dotPos == std::string::npos) {
            return std::unexpected(ChainApiError{
                ErrorCode::RefUndefined,
                ErrorClass::Schema,
                "Invalid operation id (missing dot): " + current.value});
        }
        auto resName = current.value.substr(0, dotPos);
        auto opName = current.value.substr(dotPos + 1);

        auto resIt = project.resources.find(ResourceId{resName});
        if (resIt == project.resources.end()) {
            return std::unexpected(ChainApiError{
                ErrorCode::RefUndefined,
                ErrorClass::Schema,
                "Resource not found: " + resName +
                " (referenced by operation " + current.value + ")"});
        }
        auto opIt = resIt->second.operations.find(opName);
        if (opIt == resIt->second.operations.end()) {
            return std::unexpected(ChainApiError{
                ErrorCode::RefUndefined,
                ErrorClass::Schema,
                "Operation not found: " + current.value});
        }

        const auto& op = opIt->second;

        // Gather all deps (explicit + implicit).
        std::set<OperationId> allDeps(op.explicitDependencies.begin(),
                                      op.explicitDependencies.end());
        auto implicitDeps = inferImplicitDeps(op, project);
        allDeps.insert(implicitDeps.begin(), implicitDeps.end());

        graph[current] = {allDeps.begin(), allDeps.end()};

        for (const auto& dep : allDeps) {
            if (!visited.contains(dep)) {
                worklist.push(dep);
            }
        }
    }

    // 2. Topological sort (Kahn's) with lexicographic tie-break (AC-3.1.3).
    //    Graph edges are "current depends on dep", so the sort order is
    //    dependencies first, target last.

    // Build in-degree map (how many things depend on each node).
    // We need the reverse: for Kahn's, edges go from dependency→dependent.
    std::map<OperationId, int> inDegree;
    std::map<OperationId, std::vector<OperationId>> dependents;  // dep → who depends on it

    for (const auto& [node, deps] : graph) {
        if (!inDegree.contains(node)) inDegree[node] = 0;
        for (const auto& dep : deps) {
            dependents[dep].push_back(node);
            inDegree[node]++;
            if (!inDegree.contains(dep)) inDegree[dep] = 0;
        }
    }

    // Priority queue with lexicographic ordering (smallest first = deterministic).
    auto cmp = [](const OperationId& a, const OperationId& b) {
        return a.value > b.value;  // min-heap
    };
    std::priority_queue<OperationId, std::vector<OperationId>, decltype(cmp)> ready(cmp);

    for (const auto& [node, degree] : inDegree) {
        if (degree == 0) {
            ready.push(node);
        }
    }

    std::vector<OperationId> sorted;
    sorted.reserve(graph.size());

    while (!ready.empty()) {
        auto node = ready.top();
        ready.pop();
        sorted.push_back(node);

        if (dependents.contains(node)) {
            for (const auto& dependent : dependents[node]) {
                inDegree[dependent]--;
                if (inDegree[dependent] == 0) {
                    ready.push(dependent);
                }
            }
        }
    }

    // 3. Cycle detection: if sorted.size() < graph.size(), there's a cycle.
    if (sorted.size() < graph.size()) {
        // Identify cycle members (nodes not in sorted output).
        std::set<OperationId> sortedSet(sorted.begin(), sorted.end());
        std::string cycleOps;
        for (const auto& [node, _] : graph) {
            if (!sortedSet.contains(node)) {
                if (!cycleOps.empty()) cycleOps += " → ";
                cycleOps += node.value;
            }
        }
        return std::unexpected(ChainApiError{
            ErrorCode::Cycle,
            ErrorClass::Schema,
            "Circular dependency detected: " + cycleOps});
    }

    return sorted;
}

}  // namespace chainapi::engine
