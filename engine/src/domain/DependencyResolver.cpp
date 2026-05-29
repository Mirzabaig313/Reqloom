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
/// Returns views into `op`; the caller must keep `op` alive while the
/// returned list is in use (always synchronous here).
std::vector<std::string_view> operationTemplates(const Operation& op) {
    std::vector<std::string_view> templates;
    templates.reserve(1 + (op.bodyTemplate ? 1U : 0U) + op.headers.size() + op.queryParams.size() +
                      (op.bodyForm ? op.bodyForm->size() : 0U));
    templates.emplace_back(op.pathTemplate);
    if (op.bodyTemplate) {
        templates.emplace_back(*op.bodyTemplate);
    }
    for (const auto& [_, v] : op.headers) {
        templates.emplace_back(v);
    }
    for (const auto& [_, v] : op.queryParams) {
        templates.emplace_back(v);
    }
    if (op.bodyForm) {
        for (const auto& [_, v] : *op.bodyForm) {
            templates.emplace_back(v);
        }
    }
    return templates;
}

/// Trim ASCII spaces/tabs — matches VariableResolver's trim of each
/// `{{...}}` capture.
[[nodiscard]] std::string_view trimWs(std::string_view s) {
    const auto begin = s.find_first_not_of(" \t");
    if (begin == std::string_view::npos) {
        return {};
    }
    const auto end = s.find_last_not_of(" \t");
    return s.substr(begin, end - begin + 1);
}

/// Find every `{{...}}` reference and split it into (scope, field).
///
/// Must mirror VariableResolver exactly — it matches `{{[^}]+}}`, trims,
/// then splits on the first dot. A narrower pattern here would miss
/// `{{ order.id }}` (spaces), dropping a real dependency edge and
/// letting a whitespaced cyclic reference evade detection.
///
/// `$` builtins are recorded with scope "$": the resolver handles them,
/// and anything nested in their call args resolves (or fails) at send
/// time, never at load — so "$" satisfies the scope check without
/// inventing a load error.
std::vector<ParsedRef> scanReferences(const std::vector<std::string_view>& templates) {
    static const std::regex refPattern(R"(\{\{([^}]+)\}\})");
    std::vector<ParsedRef> refs;
    for (const auto& tmpl : templates) {
        auto begin = std::cregex_iterator(tmpl.data(), tmpl.data() + tmpl.size(), refPattern);
        auto end = std::cregex_iterator();
        for (auto it = begin; it != end; ++it) {
            const auto inner = std::string{trimWs((*it)[1].str())};
            if (inner.empty()) {
                continue;
            }

            if (inner.front() == '$') {
                refs.push_back(ParsedRef{"$", {}});
                continue;
            }

            const auto dot = inner.find('.');
            if (dot == std::string::npos) {
                // No dot → unresolved at send time, not a load error. Skip.
                continue;
            }

            auto scope = inner.substr(0, dot);
            auto field = inner.substr(dot + 1);

            // Strip a trailing `[N]` index, matching resolveDotted's
            // indexed-resource handling (`order[2]`).
            if (const auto br = scope.find('['); br != std::string::npos) {
                scope = scope.substr(0, br);
            }
            refs.push_back(ParsedRef{std::move(scope), std::move(field)});
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

        if (const auto it = dependents.find(node); it != dependents.end()) {
            for (const auto& dependent : it->second) {
                inDegree[dependent]--;
                if (inDegree[dependent] == 0) {
                    ready.push(dependent);
                }
            }
        }
    }

    // 3. Cycle detection: if sorted.size() < graph.size(), there's a cycle.
    if (sorted.size() < graph.size()) {
        std::set<OperationId> const sortedSet(sorted.begin(), sorted.end());
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
    // A resolvable reference root: $ builtins, env, secret, or any
    // defined actor or resource.
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

    // Walk every operation: check references and depends_on targets, and
    // build the whole-project graph so the cycle check sees all edges,
    // not just those reachable from one target.
    std::map<OperationId, std::vector<OperationId>> graph;

    for (const auto& [resId, resource] : project.resources) {
        for (const auto& [opName, op] : resource.operations) {
            const auto opId = OperationId{std::format("{}.{}", resId.value, opName)};

            // AC-3.1.6: a reference whose scope isn't a known root can
            // never resolve — reject at load.
            for (const auto& ref : scanReferences(operationTemplates(op))) {
                if (!isKnownScope(ref.scope)) {
                    return std::unexpected(ChainApiError{
                        ErrorCode::RefUndefined,
                        ErrorClass::Schema,
                        std::format("Operation '{}' references undefined symbol '{}.{}': "
                                    "no actor, resource, env, or secret named '{}'",
                                    opId.value,
                                    ref.scope,
                                    ref.field,
                                    ref.scope)});
                }
            }

            // depends_on targets must name a real operation.
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
                    return std::unexpected(
                        ChainApiError{ErrorCode::RefUndefined,
                                      ErrorClass::Schema,
                                      std::format("Operation '{}' declares depends_on '{}', "
                                                  "which is not a defined operation",
                                                  opId.value,
                                                  dep.value)});
                }
                allDeps.insert(dep);
            }

            const auto implicit = inferImplicitDeps(op, project);
            allDeps.insert(implicit.begin(), implicit.end());
            graph[opId] = {allDeps.begin(), allDeps.end()};
        }
    }

    // Whole-project cycle detection (Kahn's). A self-loop never reaches
    // in-degree 0, so AC-3.1.5 falls out for free.
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
