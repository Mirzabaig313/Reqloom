// DependencyResolver — builds execution chain using Kahn's topological sort. Detects cycles and
// undefined refs.
#include "DependencyResolver.h"

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <queue>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace reqloom::engine {

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

/// Like `inferImplicitDeps` but keeps the flowing variable per producer
/// (as "resource.var"), so callers building a graph can label edges. A
/// producer that satisfies several referenced variables yields one entry
/// per distinct (producer, variable) pair.
std::vector<std::pair<OperationId, std::string>> inferImplicitDepsDetailed(const Operation& op,
                                                                           const Project& project) {
    std::vector<std::pair<OperationId, std::string>> deps;
    std::set<std::pair<std::string, std::string>> seen;

    for (const auto& ref : scanReferences(operationTemplates(op))) {
        const auto& refResource = ref.scope;
        const auto& refVar = ref.field;

        if (refResource == "$" || refResource == "env" || refResource == "secret") {
            continue;
        }

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
                        const auto variable = std::format("{}.{}", refResource, refVar);
                        if (seen.insert({depId.value, variable}).second) {
                            deps.emplace_back(std::move(depId), variable);
                        }
                    }
                    break;
                }
            }
        }
    }

    return deps;
}

/// Extract all implicit dependencies from an operation's templates.
/// A reference like `{{product.product_id}}` implies a dependency on
/// the resource "product" — specifically on whatever operation produces
/// that variable (i.e. has it in `extract:`).
std::vector<OperationId> inferImplicitDeps(const Operation& op, const Project& project) {
    std::set<OperationId> deps;
    for (auto& [producer, _] : inferImplicitDepsDetailed(op, project)) {
        deps.insert(producer);
    }
    return {deps.begin(), deps.end()};
}

}  // namespace

DependencyResolver::DependencyResolver() = default;

std::expected<std::vector<OperationId>, ReqloomError> DependencyResolver::resolve(
    const Project& project, const OperationId& target) const {
    auto plan = resolvePlan(project, target);
    if (!plan) {
        return std::unexpected(plan.error());
    }
    return std::move(plan->order);
}

std::expected<ResolvedPlan, ReqloomError> DependencyResolver::resolvePlan(
    const Project& project, const OperationId& target) const {
    // 1. Build the full dependency graph (explicit + implicit edges) for the
    //    transitive closure of `target`, recording edge metadata as we go.
    std::map<OperationId, std::vector<OperationId>> graph;
    std::vector<DependencyEdge> edges;
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
                ReqloomError{ErrorCode::RefUndefined,
                             ErrorClass::Schema,
                             "Invalid operation id (missing dot): " + current.value});
        }
        auto resName = current.value.substr(0, dotPos);
        auto opName = current.value.substr(dotPos + 1);

        auto resIt = project.resources.find(ResourceId{resName});
        if (resIt == project.resources.end()) {
            return std::unexpected(ReqloomError{ErrorCode::RefUndefined,
                                                ErrorClass::Schema,
                                                "Resource not found: " + resName +
                                                    " (referenced by operation " + current.value +
                                                    ")"});
        }
        auto opIt = resIt->second.operations.find(opName);
        if (opIt == resIt->second.operations.end()) {
            return std::unexpected(ReqloomError{ErrorCode::RefUndefined,
                                                ErrorClass::Schema,
                                                "Operation not found: " + current.value});
        }

        const auto& op = opIt->second;

        // Explicit edges first, so an explicit dep that is also implied is
        // surfaced as the (solid) explicit edge rather than a duplicate.
        // Iterate the de-duplicated set so a repeated `depends_on` yields one
        // edge, not two.
        std::set<OperationId> explicitProducers(op.explicitDependencies.begin(),
                                                op.explicitDependencies.end());
        std::set<OperationId> allDeps = explicitProducers;
        for (const auto& dep : explicitProducers) {
            edges.push_back(DependencyEdge{current, dep, false, {}});
        }
        for (auto& [producer, variable] : inferImplicitDepsDetailed(op, project)) {
            allDeps.insert(producer);
            if (!explicitProducers.contains(producer)) {
                edges.push_back(DependencyEdge{current, producer, true, std::move(variable)});
            }
        }

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
        return std::unexpected(ReqloomError{
            ErrorCode::Cycle, ErrorClass::Schema, "Circular dependency detected: " + cycleOps});
    }

    // Stable edge order so renderers and tests see deterministic output.
    std::ranges::sort(edges, [](const DependencyEdge& a, const DependencyEdge& b) {
        if (a.consumer.value != b.consumer.value) {
            return a.consumer.value < b.consumer.value;
        }
        return a.producer.value < b.producer.value;
    });

    return ResolvedPlan{std::move(sorted), std::move(edges)};
}

std::expected<void, ReqloomError> DependencyResolver::validate(const Project& project) const {
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
                    return std::unexpected(ReqloomError{
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
                        ReqloomError{ErrorCode::RefUndefined,
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
        return std::unexpected(ReqloomError{
            ErrorCode::Cycle, ErrorClass::Schema, "Circular dependency detected: " + cycleOps});
    }

    return {};
}

std::vector<std::string> DependencyResolver::collectSecretReferences(const Project& project) {
    std::set<std::string> names;

    // Scan a list of template strings and record every `secret.X` field.
    const auto scan = [&names](const std::vector<std::string_view>& templates) {
        for (const auto& ref : scanReferences(templates)) {
            if (ref.scope == "secret" && !ref.field.empty()) {
                names.insert(ref.field);
            }
        }
    };

    for (const auto& [_, resource] : project.resources) {
        for (const auto& [__, op] : resource.operations) {
            scan(operationTemplates(op));
            if (op.pollUntil) {
                std::vector<std::string_view> pollTemplates{op.pollUntil->pathTemplate,
                                                            op.pollUntil->successWhen};
                if (op.pollUntil->failWhen) {
                    pollTemplates.emplace_back(*op.pollUntil->failWhen);
                }
                scan(pollTemplates);
            }
        }
    }

    // Environment values can themselves be secret references — a
    // `key: !secret NAME` env entry is parsed into `{{secret.NAME}}`, and
    // an operation that references `{{env.key}}` expands to it. Scan them
    // so NAME is pre-loaded from the secret store.
    for (const auto& [_, vars] : project.environments) {
        std::vector<std::string_view> envValues;
        envValues.reserve(vars.size());
        for (const auto& [__, value] : vars) {
            envValues.emplace_back(value);
        }
        scan(envValues);
    }

    // Actor auth surfaces also resolve `{{secret.X}}` (basic credentials,
    // api_key, oauth client secrets, signing keys, refresh bodies, and
    // injected headers like `Authorization: Bearer {{secret.TOKEN}}`).
    for (const auto& [_, actor] : project.actors) {
        std::vector<std::string_view> actorTemplates;
        for (const auto& [__, v] : actor.authConfig) {
            actorTemplates.emplace_back(v);
        }
        for (const auto& [__, v] : actor.inject.headers) {
            actorTemplates.emplace_back(v);
        }
        for (const auto& step : actor.authSteps) {
            actorTemplates.emplace_back(step.pathTemplate);
            if (step.bodyTemplate) {
                actorTemplates.emplace_back(*step.bodyTemplate);
            }
            for (const auto& [__, v] : step.headers) {
                actorTemplates.emplace_back(v);
            }
        }
        if (actor.refresh) {
            actorTemplates.emplace_back(actor.refresh->pathTemplate);
            if (actor.refresh->bodyTemplate) {
                actorTemplates.emplace_back(*actor.refresh->bodyTemplate);
            }
            for (const auto& [__, v] : actor.refresh->headers) {
                actorTemplates.emplace_back(v);
            }
        }
        scan(actorTemplates);
    }

    return {names.begin(), names.end()};
}

std::vector<VariableSuggestion> collectVariableSuggestions(const Project& project,
                                                           const OperationId& target,
                                                           const ResolvedPlan& plan,
                                                           const std::string& environment) {
    std::vector<VariableSuggestion> out;
    std::set<std::string> seen;
    const auto add = [&out, &seen](
                         std::string token, VariableSuggestion::Kind kind, std::string detail) {
        if (token.empty() || !seen.insert(token).second) {
            return;
        }
        out.push_back(VariableSuggestion{std::move(token), kind, std::move(detail)});
    };

    // Resolve "<resource>.<op>" against the project.
    const auto findOp = [&project](const OperationId& id) -> const Operation* {
        const auto dot = id.value.find('.');
        if (dot == std::string::npos) {
            return nullptr;
        }
        const auto resName = id.value.substr(0, dot);
        const auto opName = id.value.substr(dot + 1);
        const auto resIt = project.resources.find(ResourceId{resName});
        if (resIt == project.resources.end()) {
            return nullptr;
        }
        const auto opIt = resIt->second.operations.find(opName);
        if (opIt == resIt->second.operations.end()) {
            return nullptr;
        }
        return &opIt->second;
    };

    // 1. Upstream extracts — every producing op in the chain (not the target).
    //    A variable extracted by an op in resource R is referenced as {{R.var}}.
    for (const auto& opId : plan.order) {
        if (opId.value == target.value) {
            continue;
        }
        const auto* op = findOp(opId);
        if (op == nullptr) {
            continue;
        }
        const auto dot = opId.value.find('.');
        const std::string resource =
            dot != std::string::npos ? opId.value.substr(0, dot) : opId.value;
        for (const auto& ext : op->extractions) {
            add(resource + "." + ext.variableName, VariableSuggestion::Kind::Extract, opId.value);
        }
    }

    // 2. The target actor's session tokens — {{actor.var}} from its auth
    //    steps and refresh block.
    if (const auto* targetOp = findOp(target); targetOp != nullptr) {
        if (const auto actorIt = project.actors.find(targetOp->actor);
            actorIt != project.actors.end()) {
            const auto& actor = actorIt->second;
            const auto& actorId = actor.id.value;
            const auto addTokens = [&](const std::vector<Extraction>& exts) {
                for (const auto& ext : exts) {
                    add(actorId + "." + ext.variableName,
                        VariableSuggestion::Kind::ActorToken,
                        actorId);
                }
            };
            for (const auto& step : actor.authSteps) {
                addTokens(step.extractions);
            }
            if (actor.refresh) {
                addTokens(actor.refresh->extractions);
            }
        }
    }

    // 3. Environment variables — {{env.X}}.
    const std::string envName = environment.empty() ? project.defaultEnvironment : environment;
    if (const auto envIt = project.environments.find(envName);
        envIt != project.environments.end()) {
        for (const auto& [key, _] : envIt->second) {
            add("env." + key, VariableSuggestion::Kind::EnvVar, envName);
        }
    }

    // 4. Declared secrets — {{secret.X}}.
    for (const auto& name : DependencyResolver::collectSecretReferences(project)) {
        add("secret." + name, VariableSuggestion::Kind::Secret, {});
    }

    // 5. Built-ins. Function forms are inserted with empty parens for the user
    //    to fill; `$.now` accepts an optional +/- duration offset.
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 8> kBuiltins{{
        {"$.uuid", "Random UUID v4"},
        {"$.now", "Current timestamp (ISO 8601); supports an offset, e.g. $.now+5m"},
        {"$.base64.encode()", "Base64-encode the argument"},
        {"$.base64.decode()", "Base64-decode the argument"},
        {"$.hex.encode()", "Hex-encode the argument"},
        {"$.hex.decode()", "Hex-decode the argument"},
        {"$.url.encode()", "URL-encode the argument"},
        {"$.url.decode()", "URL-decode the argument"},
    }};
    for (const auto& [token, desc] : kBuiltins) {
        add(std::string{token}, VariableSuggestion::Kind::Builtin, std::string{desc});
    }

    return out;
}

}  // namespace reqloom::engine
