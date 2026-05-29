// DependencyResolver — builds execution chain using Kahn's topological sort. Detects cycles and
// undefined refs.
#include "DependencyResolver.h"

#include <format>
#include <queue>
#include <regex>
#include <set>

namespace chainapi::engine {

namespace {

/// One parsed `{{scope.field}}` reference found in a template. `scope`
/// is the part before the first dot with any `[N]` index stripped; for
/// `{{order[2].order_id}}` scope is "order", field is "order_id".
struct ParsedRef {
    std::string scope;
    std::string field;
};

/// Collect the templated strings on an operation (path, body, headers,
/// query params, form fields) into one list for reference scanning.
std::vector<std::string> operationTemplates(const Operation& op) {
    std::vector<std::string> templates;
    templates.push_back(op.pathTemplate);
    if (op.bodyTemplate) {
        templates.push_back(*op.bodyTemplate);
    }
    for (const auto& [_, v] : op.headers) {
        templates.push_back(v);
    }
    for (const auto& [_, v] : op.queryParams) {
        templates.push_back(v);
    }
    if (op.bodyForm) {
        for (const auto& [_, v] : *op.bodyForm) {
            templates.push_back(v);
        }
    }
    return templates;
}

/// Find every `{{X.y}}` / `{{X[N].y}}` reference across a set of
/// templates. The first capture group is the scope (resource / actor /
/// env / secret), the second is the field. `$`-prefixed builtins are
/// matched separately by the resolver and excluded here.
std::vector<ParsedRef> scanReferences(const std::vector<std::string>& templates) {
    // Mirrors VariableResolver's grammar. Kept in sync deliberately:
    // parse-time validation must accept exactly what runtime resolves.
    static const std::regex refPattern(R"(\{\{(\w+)(?:\[\d+\])?\.(\w+)\}\})");
    std::vector<ParsedRef> refs;
    for (const auto& tmpl : templates) {
        auto begin = std::sregex_iterator(tmpl.begin(), tmpl.end(), refPattern);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            refs.push_back(ParsedRef{(*it)[1].str(), (*it)[2].str()});
        }
    }
    return refs;
}

/// Extract all implicit dependencies from an operation's templates.
/// A reference like `{{product.product_id}}` implies a dependency on
/// the resource "product" — specifically on whatever operation produces
/// that variable (i.e. has it in `extract:`).
std::vector<OperationId> inferImplicitDeps(const Operation& op, const Project& project) {
    std::set<OperationId> deps;

    for (const auto& ref : scanReferences(operationTemplates(op))) {
        const auto& refResource = ref.scope;
        const auto& refVar = ref.field;

        if (refResource == "$" || refResource == "env" || refResource == "secret") {
            continue;
        }

        // Actor deps are handled by the session system, not the chain.
        bool isActor = false;
        for (const auto& [actorId, _] : project.actors) {
            if (actorId.value == refResource) {
                isActor = true;
                break;
            }
        }
        if (isActor) {
            continue;
        }

        auto resIt = project.resources.find(ResourceId{refResource});
        if (resIt == project.resources.end()) {
            continue;
        }

        for (const auto& [opName, resOp] : resIt->second.operations) {
            for (const auto& ext : resOp.extractions) {
                if (ext.variableName == refVar) {
                    auto depId = OperationId{std::format("{}.{}", refResource, opName)};
                    if (depId.value != op.id.value) {
                        deps.insert(depId);
                    }
                    break;
                }
            }
        }
    }

    return {deps.begin(), deps.end()};
}

}  // namespace

DependencyResolver::DependencyResolver() = default;

std::expected<std::vector<OperationId>, ChainApiError> DependencyResolver::resolve(
    const Project& project, const OperationId& target) const {
    // 1. Build the full dependency graph (explicit + implicit edges) for the
    //    transitive closure of `target`.
    std::map<OperationId, std::vector<OperationId>> graph;
    std::set<OperationId> visited;
    std::queue<OperationId> worklist;
    worklist.push(target);

    while (!worklist.empty()) {
        auto current = worklist.front();
        worklist.pop();

        if (visited.contains(current)) {
            continue;
        }
        visited.insert(current);

        auto dotPos = current.value.find('.');
        if (dotPos == std::string::npos) {
            return std::unexpected(
                ChainApiError{ErrorCode::RefUndefined,
                              ErrorClass::Schema,
                              "Invalid operation id (missing dot): " + current.value});
        }
        auto resName = current.value.substr(0, dotPos);
        auto opName = current.value.substr(dotPos + 1);

        auto resIt = project.resources.find(ResourceId{resName});
        if (resIt == project.resources.end()) {
            return std::unexpected(ChainApiError{ErrorCode::RefUndefined,
                                                 ErrorClass::Schema,
                                                 "Resource not found: " + resName +
                                                     " (referenced by operation " + current.value +
                                                     ")"});
        }
        auto opIt = resIt->second.operations.find(opName);
        if (opIt == resIt->second.operations.end()) {
            return std::unexpected(ChainApiError{ErrorCode::RefUndefined,
                                                 ErrorClass::Schema,
                                                 "Operation not found: " + current.value});
        }

        const auto& op = opIt->second;

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

    // 2. Topological sort (Kahn's) with lexicographic tie-break.
    //    Graph edges are "current depends on dep", so the sort order is
    //    dependencies first, target last.
    std::map<OperationId, int> inDegree;
    std::map<OperationId, std::vector<OperationId>> dependents;

    for (const auto& [node, deps] : graph) {
        if (!inDegree.contains(node)) {
            inDegree[node] = 0;
        }
        for (const auto& dep : deps) {
            dependents[dep].push_back(node);
            inDegree[node]++;
            if (!inDegree.contains(dep)) {
                inDegree[dep] = 0;
            }
        }
    }

    auto cmp = [](const OperationId& a, const OperationId& b) {
        return a.value > b.value;  // min-heap for lexicographic order
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
        std::set<OperationId> sortedSet(sorted.begin(), sorted.end());
        std::string cycleOps;
        for (const auto& [node, _] : graph) {
            if (!sortedSet.contains(node)) {
                if (!cycleOps.empty()) {
                    cycleOps += " → ";
                }
                cycleOps += node.value;
            }
        }
        return std::unexpected(ChainApiError{
            ErrorCode::Cycle, ErrorClass::Schema, "Circular dependency detected: " + cycleOps});
    }

    return sorted;
}

std::expected<void, ChainApiError> DependencyResolver::validate(const Project& project) const {
    // Helper: is `scope` a resolvable reference root? `$` builtins,
    // env, secret, any defined actor, or any defined resource.
    const auto isKnownScope = [&](const std::string& scope) -> bool {
        if (scope == "$" || scope == "env" || scope == "secret") {
            return true;
        }
        for (const auto& [actorId, _] : project.actors) {
            if (actorId.value == scope) {
                return true;
            }
        }
        return project.resources.contains(ResourceId{scope});
    };

    // 1. Reference + dependency checks across every operation. Build the
    //    whole-project graph as we go so the cycle check below sees all
    //    edges, not just those reachable from one target.
    std::map<OperationId, std::vector<OperationId>> graph;

    for (const auto& [resId, resource] : project.resources) {
        for (const auto& [opName, op] : resource.operations) {
            const auto opId = OperationId{std::format("{}.{}", resId.value, opName)};

            // 1a. Undefined-reference check. A reference whose scope is
            //     not a known actor / resource / env / secret / builtin
            //     can never resolve — reject at load (AC-3.1.6).
            for (const auto& ref : scanReferences(operationTemplates(op))) {
                if (!isKnownScope(ref.scope)) {
                    return std::unexpected(ChainApiError{
                        ErrorCode::RefUndefined,
                        ErrorClass::Schema,
                        std::format("Operation '{}' references undefined symbol '{}.{}': "
                                    "no actor, resource, env, or secret named '{}'",
                                    opId.value, ref.scope, ref.field, ref.scope)});
                }
            }

            // 1b. Explicit `depends_on:` targets must exist.
            std::set<OperationId> allDeps;
            for (const auto& dep : op.explicitDependencies) {
                const auto dotPos = dep.value.find('.');
                bool exists = false;
                if (dotPos != std::string::npos) {
                    const auto depRes = dep.value.substr(0, dotPos);
                    const auto depOp = dep.value.substr(dotPos + 1);
                    auto resIt = project.resources.find(ResourceId{depRes});
                    if (resIt != project.resources.end()) {
                        exists = resIt->second.operations.contains(depOp);
                    }
                }
                if (!exists) {
                    return std::unexpected(ChainApiError{
                        ErrorCode::RefUndefined,
                        ErrorClass::Schema,
                        std::format("Operation '{}' declares depends_on '{}', "
                                    "which is not a defined operation",
                                    opId.value, dep.value)});
                }
                allDeps.insert(dep);
            }

            const auto implicit = inferImplicitDeps(op, project);
            allDeps.insert(implicit.begin(), implicit.end());
            graph[opId] = {allDeps.begin(), allDeps.end()};
        }
    }

    // 2. Whole-project cycle detection via Kahn's topological sort. A
    //    self-loop (op depends on itself) shows up as a node that never
    //    reaches in-degree 0.
    std::map<OperationId, int> inDegree;
    std::map<OperationId, std::vector<OperationId>> dependents;
    for (const auto& [node, deps] : graph) {
        inDegree.try_emplace(node, 0);
        for (const auto& dep : deps) {
            dependents[dep].push_back(node);
            inDegree[node]++;
            inDegree.try_emplace(dep, 0);
        }
    }

    std::queue<OperationId> ready;
    for (const auto& [node, degree] : inDegree) {
        if (degree == 0) {
            ready.push(node);
        }
    }

    std::size_t processed = 0;
    while (!ready.empty()) {
        auto node = ready.front();
        ready.pop();
        ++processed;
        if (dependents.contains(node)) {
            for (const auto& dependent : dependents[node]) {
                if (--inDegree[dependent] == 0) {
                    ready.push(dependent);
                }
            }
        }
    }

    if (processed < inDegree.size()) {
        std::string cycleOps;
        for (const auto& [node, degree] : inDegree) {
            if (degree > 0) {
                if (!cycleOps.empty()) {
                    cycleOps += " → ";
                }
                cycleOps += node.value;
            }
        }
        return std::unexpected(ChainApiError{
            ErrorCode::Cycle, ErrorClass::Schema, "Circular dependency detected: " + cycleOps});
    }

    return {};
}

}  // namespace chainapi::engine
