// DependencyResolver — builds execution chain using Kahn's topological sort. Detects cycles and
// undefined refs.
#include "DependencyResolver.h"

#include <reqloom/engine/Predicate.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <iterator>
#include <map>
#include <optional>
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

/// Collect the templated strings on an operation's request into one list for reference
/// scanning. Views into `op`; the caller keeps `op` alive.
std::vector<std::string_view> requestTemplates(const Operation& op) {
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

void appendInlineAuthTemplates(const InlineAuth& a, std::vector<std::string_view>& out) {
    for (const std::string* field :
         {&a.token,           &a.username,           &a.password,
          &a.apiKeyName,      &a.apiKeyValue,        &a.awsAccessKey,
          &a.awsSecretKey,    &a.awsRegion,          &a.awsService,
          &a.awsSessionToken, &a.oauthConsumerKey,   &a.oauthConsumerSecret,
          &a.oauthToken,      &a.oauthTokenSecret,   &a.oauth2TokenUrl,
          &a.oauth2ClientId,  &a.oauth2ClientSecret, &a.oauth2Scope,
          &a.oauth2AuthUrl,   &a.oauth2CallbackUrl,  &a.oauth2AccessToken,
          &a.jwtSecret,       &a.jwtPayload,         &a.mtlsCertPath,
          &a.mtlsKeyPath,     &a.mtlsKeyPassword,    &a.mtlsCaCertPath}) {
        if (!field->empty()) {
            out.emplace_back(*field);
        }
    }
}

/// The operation's effective inline auth templates: its own block, or the project default
/// when it is `Inherit`.
std::vector<std::string_view> authTemplates(const Operation& op, const Project& project) {
    std::vector<std::string_view> templates;
    if (!op.inlineAuth) {
        return templates;
    }
    if (op.inlineAuth->type == InlineAuthType::Inherit) {
        if (project.defaultAuth && project.defaultAuth->type != InlineAuthType::Inherit) {
            appendInlineAuthTemplates(*project.defaultAuth, templates);
        }
        return templates;
    }
    appendInlineAuthTemplates(*op.inlineAuth, templates);
    return templates;
}

std::vector<std::string_view> pollTemplates(const Operation& op) {
    std::vector<std::string_view> templates;
    if (op.pollUntil) {
        templates.emplace_back(op.pollUntil->pathTemplate);
        templates.emplace_back(op.pollUntil->successWhen);
        if (op.pollUntil->failWhen) {
            templates.emplace_back(*op.pollUntil->failWhen);
        }
    }
    return templates;
}

std::vector<std::string_view> operationTemplates(const Operation& op, const Project& project) {
    auto templates = requestTemplates(op);
    std::ranges::copy(authTemplates(op, project), std::back_inserter(templates));
    std::ranges::copy(pollTemplates(op), std::back_inserter(templates));
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

/// Split one dotted reference into a ParsedRef; no dot → nullopt (unresolved at send time,
/// not a load error).
std::optional<ParsedRef> parseDotted(std::string_view inner) {
    const auto dot = inner.find('.');
    if (dot == std::string_view::npos) {
        return std::nullopt;
    }
    auto scope = std::string{inner.substr(0, dot)};
    auto field = std::string{inner.substr(dot + 1)};

    if (const auto br = scope.find('['); br != std::string::npos) {
        scope = scope.substr(0, br);
    }
    return ParsedRef{std::move(scope), std::move(field)};
}

/// The bare-reference argument of a builtin call `$.ns.name(arg)`, if any; a quoted argument
/// is a literal.
std::optional<std::string_view> builtinCallArgument(std::string_view inner) {
    const auto open = inner.find('(');
    if (open == std::string_view::npos || inner.back() != ')') {
        return std::nullopt;
    }
    const auto arg = trimWs(inner.substr(open + 1, inner.size() - open - 2));
    if (arg.empty() || arg.front() == '"' || arg.front() == '\'') {
        return std::nullopt;
    }
    return arg;
}

/// Find every `{{...}}` reference and split it into (scope, field).
///
/// Must mirror VariableResolver exactly — it matches `{{[^}]+}}`, trims,
/// then splits on the first dot. A narrower pattern here would miss
/// `{{ order.id }}` (spaces), dropping a real dependency edge and
/// letting a whitespaced cyclic reference evade detection.
///
/// `$` builtins are recorded with scope "$". A builtin call's bare reference argument is
/// recorded too, so it is checked, creates edges and has its secret pre-loaded.
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
                // The builtin's tail is kept so validate() can check `$.env` names against
                // allowed_env.
                refs.push_back(
                    ParsedRef{"$", inner.starts_with("$.") ? inner.substr(2) : std::string{}});
                if (const auto arg = builtinCallArgument(inner)) {
                    if (auto ref = parseDotted(*arg)) {
                        refs.push_back(std::move(*ref));
                    }
                }
                continue;
            }

            if (auto ref = parseDotted(inner)) {
                refs.push_back(std::move(*ref));
            }
        }
    }
    return refs;
}

/// Reject a `$.env.NAME` builtin whose NAME is not in the project's `allowed_env`.
std::expected<void, ReqloomError> checkEnvBuiltinAllowed(const OperationId& opId,
                                                         const ParsedRef& ref,
                                                         const Project& project) {
    constexpr std::string_view kEnvPrefix{"env."};
    if (ref.scope != "$" || !ref.field.starts_with(kEnvPrefix)) {
        return {};
    }
    auto name = std::string_view{ref.field}.substr(kEnvPrefix.size());
    name = name.substr(0, name.find_first_of(" \t"));
    if (std::ranges::find(project.allowedEnv, name) != project.allowedEnv.end()) {
        return {};
    }
    return std::unexpected(ReqloomError{
        ErrorCode::RefUndefined,
        ErrorClass::Schema,
        std::format("Operation '{}' reads $.env.{}, but environment variable {} is not in "
                    "allowed_env in reqloom.yaml; add it to the allowed_env list there",
                    opId.value,
                    name,
                    name)});
}

/// The faker generators VariableResolver implements; any other name never resolves.
constexpr std::array<std::string_view, 2> kFakerNames{"email", "phone"};

/// Reject `$.faker.NAME` for an unknown generator; anything after the name is left to run time.
std::expected<void, ReqloomError> checkFakerBuiltinKnown(const OperationId& opId,
                                                         const ParsedRef& ref) {
    constexpr std::string_view kFakerPrefix{"faker."};
    if (ref.scope != "$" || !ref.field.starts_with(kFakerPrefix)) {
        return {};
    }
    auto name = std::string_view{ref.field}.substr(kFakerPrefix.size());
    const auto end = std::ranges::find_if(name, [](const char c) {
        return std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '_';
    });
    name = name.substr(0, static_cast<std::size_t>(end - name.begin()));
    if (std::ranges::find(kFakerNames, name) != kFakerNames.end()) {
        return {};
    }
    return std::unexpected(
        ReqloomError{ErrorCode::RefUndefined,
                     ErrorClass::Schema,
                     std::format("Operation '{}' uses $.faker.{}, which is not a faker generator; "
                                 "valid names: email, phone",
                                 opId.value,
                                 name)});
}

/// True when `expr` has a `{{` outside a quoted string; such a predicate is left to run time
/// rather than rejected at load.
[[nodiscard]] bool hasUnquotedTemplate(std::string_view expr) {
    char quote = 0;
    for (std::size_t i = 0; i < expr.size(); ++i) {
        const char c = expr[i];
        if (quote != 0) {
            if (c == '\\') {
                ++i;
            } else if (c == quote) {
                quote = 0;
            }
        } else if (c == '\'' || c == '"') {
            quote = c;
        } else if (c == '{' && i + 1 < expr.size() && expr[i + 1] == '{') {
            return true;
        }
    }
    return false;
}

/// Parse every predicate an operation declares and check every regex extraction pattern, so
/// a malformed one is a load error instead of a run-time failure after a request has gone out.
std::expected<void, ReqloomError> validateOperationExpressions(const OperationId& opId,
                                                               const Operation& op) {
    const auto reject = [&](const std::string& field, const ReqloomError& err) {
        return std::unexpected(
            ReqloomError{ErrorCode::SchemaInvalid,
                         ErrorClass::Schema,
                         std::format("Operation '{}' {}: {}", opId.value, field, err.detail)});
    };
    const auto check = [&](const std::string& field,
                           std::string_view expr) -> std::expected<void, ReqloomError> {
        if (hasUnquotedTemplate(expr)) {
            return {};
        }
        if (auto ok = validatePredicate(expr); !ok) {
            return reject(field, ok.error());
        }
        return {};
    };
    for (std::size_t i = 0; i < op.assertions.size(); ++i) {
        const auto& a = op.assertions[i];
        const auto label =
            a.name ? std::format("assert[{}] '{}'", i, *a.name) : std::format("assert[{}]", i);
        if (auto ok = check(label, a.expr); !ok) {
            return ok;
        }
    }
    if (op.pollUntil) {
        if (auto ok = check("poll_until.success_when", op.pollUntil->successWhen); !ok) {
            return ok;
        }
        if (op.pollUntil->failWhen) {
            if (auto ok = check("poll_until.fail_when", *op.pollUntil->failWhen); !ok) {
                return ok;
            }
        }
    }
    for (const auto& ext : op.extractions) {
        if (ext.source != Extraction::Source::Regex) {
            continue;
        }
        if (auto ok = validateRegexPattern(ext.sourcePath); !ok) {
            return reject(std::format("extract '{}' (regex)", ext.variableName), ok.error());
        }
    }
    return {};
}

using ImplicitDeps = std::vector<std::pair<OperationId, std::string>>;

[[nodiscard]] bool consumesVariable(const Operation& op,
                                    const Project& project,
                                    const std::string& resource,
                                    const std::string& var) {
    return std::ranges::any_of(
        scanReferences(operationTemplates(op, project)),
        [&](const ParsedRef& r) { return r.scope == resource && r.field == var; });
}

[[nodiscard]] std::string joinIds(const std::vector<OperationId>& ids) {
    std::string out;
    for (const auto& id : ids) {
        out += (out.empty() ? "" : ", ") + id.value;
    }
    return out;
}

/// Pick the producer(s) of `{{resource.var}}` for `self`: extractors other than `self`,
/// dropping any that also consume the variable. Several left → those in `self`'s depends_on,
/// else an ambiguity error. None left → "no producer". No extractor at all → no edge (a
/// runtime concern).
std::expected<std::vector<OperationId>, ReqloomError> selectProducers(const Operation& op,
                                                                      const OperationId& self,
                                                                      const Project& project,
                                                                      const Resource& resource,
                                                                      const std::string& refRes,
                                                                      const std::string& refVar) {
    std::vector<OperationId> extractors;
    std::vector<OperationId> candidates;
    for (const auto& [opName, resOp] : resource.operations) {
        auto id = OperationId{std::format("{}.{}", refRes, opName)};
        const bool extracts = std::ranges::any_of(
            resOp.extractions, [&](const Extraction& e) { return e.variableName == refVar; });
        if (id == self || !extracts) {
            continue;
        }
        extractors.push_back(id);
        if (!consumesVariable(resOp, project, refRes, refVar)) {
            candidates.push_back(std::move(id));
        }
    }
    if (extractors.empty() || candidates.size() == 1) {
        return candidates;
    }
    if (candidates.empty()) {
        return std::unexpected(ReqloomError{
            ErrorCode::RefUndefined,
            ErrorClass::Schema,
            std::format("Operation '{}': no producer for {{{{{}.{}}}}}: every operation that "
                        "extracts it ({}) also consumes it",
                        self.value,
                        refRes,
                        refVar,
                        joinIds(extractors))});
    }
    std::vector<OperationId> named;
    std::ranges::copy_if(candidates, std::back_inserter(named), [&](const OperationId& c) {
        return std::ranges::find(op.explicitDependencies, c) != op.explicitDependencies.end();
    });
    if (!named.empty()) {
        return named;
    }
    return std::unexpected(ReqloomError{
        ErrorCode::RefUndefined,
        ErrorClass::Schema,
        std::format("Operation '{}': ambiguous producer for {{{{{}.{}}}}}: candidates {} — "
                    "add depends_on",
                    self.value,
                    refRes,
                    refVar,
                    joinIds(candidates))});
}

/// Implicit dependencies of `op`, keeping the flowing variable per producer (as
/// "resource.var") so callers can label edges.
std::expected<ImplicitDeps, ReqloomError> inferImplicitDepsDetailed(const Operation& op,
                                                                    const OperationId& self,
                                                                    const Project& project) {
    ImplicitDeps deps;
    std::set<std::string> seenVariables;

    for (const auto& ref : scanReferences(operationTemplates(op, project))) {
        if (ref.scope == "$" || ref.scope == "env" || ref.scope == "secret" ||
            project.actors.contains(ActorId{ref.scope})) {
            continue;
        }
        auto resIt = project.resources.find(ResourceId{ref.scope});
        const auto variable = std::format("{}.{}", ref.scope, ref.field);
        if (resIt == project.resources.end() || !seenVariables.insert(variable).second) {
            continue;
        }
        auto producers = selectProducers(op, self, project, resIt->second, ref.scope, ref.field);
        if (!producers) {
            return std::unexpected(std::move(producers.error()));
        }
        for (auto& producer : *producers) {
            deps.emplace_back(std::move(producer), variable);
        }
    }

    return deps;
}

/// D-04: an operation's `actor`, `poll_until.actor` and `for_each.over` must name things
/// that exist.
std::expected<void, ReqloomError> validateOperationTargets(const OperationId& opId,
                                                           const Operation& op,
                                                           const Project& project) {
    const auto missing = [&](std::string_view what, const std::string& id) {
        return std::unexpected(ReqloomError{
            ErrorCode::RefUndefined,
            ErrorClass::Schema,
            std::format(
                "Operation '{}' names {} '{}', which is not defined", opId.value, what, id)});
    };
    if (!op.actor.value.empty() && !project.actors.contains(op.actor)) {
        return missing("actor", op.actor.value);
    }
    if (op.pollUntil && op.pollUntil->actor && !op.pollUntil->actor->value.empty() &&
        !project.actors.contains(*op.pollUntil->actor)) {
        return missing("poll_until actor", op.pollUntil->actor->value);
    }
    if (op.forEach && !project.resources.contains(op.forEach->over)) {
        return missing("for_each resource", op.forEach->over.value);
    }
    return {};
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
        auto implicit = inferImplicitDepsDetailed(op, current, project);
        if (!implicit) {
            return std::unexpected(std::move(implicit.error()));
        }
        for (auto& [producer, variable] : *implicit) {
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
            // `poll_until` templates may also use `{{response.X}}`; everything else must be a known
            // root.
            auto strictTemplates = requestTemplates(op);
            std::ranges::copy(authTemplates(op, project), std::back_inserter(strictTemplates));
            auto refs = scanReferences(strictTemplates);
            for (auto& ref : scanReferences(pollTemplates(op))) {
                if (ref.scope != "response") {
                    refs.push_back(std::move(ref));
                }
            }
            for (const auto& ref : refs) {
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
                if (auto denied = checkEnvBuiltinAllowed(opId, ref, project); !denied) {
                    return denied;
                }
                if (auto unknown = checkFakerBuiltinKnown(opId, ref); !unknown) {
                    return unknown;
                }
            }
            if (auto exprs = validateOperationExpressions(opId, op); !exprs) {
                return exprs;
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

            if (auto targets = validateOperationTargets(opId, op, project); !targets) {
                return targets;
            }
            const auto implicit = inferImplicitDepsDetailed(op, opId, project);
            if (!implicit) {
                return std::unexpected(implicit.error());
            }
            for (const auto& [producer, _] : *implicit) {
                allDeps.insert(producer);
            }
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
            scan(operationTemplates(op, project));
        }
    }

    if (project.defaultAuth) {
        std::vector<std::string_view> defaultAuthTemplates;
        appendInlineAuthTemplates(*project.defaultAuth, defaultAuthTemplates);
        scan(defaultAuthTemplates);
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
