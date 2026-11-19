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
#include <ranges>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace reqloom::engine {

namespace {

struct ParsedRef {
    std::string scope;
    std::string field;
};

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

std::vector<std::string_view> assertionTemplates(const Operation& op) {
    std::vector<std::string_view> templates;
    templates.reserve(op.assertions.size());
    for (const auto& assertion : op.assertions) {
        templates.emplace_back(assertion.expr);
    }
    return templates;
}

std::vector<std::string_view> operationTemplates(const Operation& op, const Project& project) {
    auto templates = requestTemplates(op);
    std::ranges::copy(authTemplates(op, project), std::back_inserter(templates));
    std::ranges::copy(pollTemplates(op), std::back_inserter(templates));
    std::ranges::copy(assertionTemplates(op), std::back_inserter(templates));
    return templates;
}

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

/// Find every `{{...}}` reference and split it into (scope, field). Must mirror VariableResolver's
/// pattern, trim and split, or a whitespaced reference would drop a dependency edge.
/// `$` builtins use scope "$"; a builtin's bare reference argument is recorded too.
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
                // Keep the builtin's tail so validate() can check `$.env` names.
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
std::expected<void, ReqloomError> checkEnvBuiltinAllowed(std::string_view owner,
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
    ReqloomError error{
        ErrorCode::RefUndefined,
        ErrorClass::Schema,
        std::format("{} reads $.env.{}, but environment variable {} is not in "
                    "allowed_env in reqloom.yaml; add it to the allowed_env list there",
                    owner,
                    name,
                    name)};
    error.reference = std::format("{{{{$.env.{}}}}}", name);
    error.fix = std::format("Add {} to the allowed_env list in reqloom.yaml.", name);
    return std::unexpected(std::move(error));
}

std::vector<std::string_view> actorTemplates(const Actor& actor) {
    std::vector<std::string_view> templates;
    for (const auto& [_, v] : actor.authConfig) {
        templates.emplace_back(v);
    }
    for (const auto& [_, v] : actor.inject.headers) {
        templates.emplace_back(v);
    }
    for (const auto& step : actor.authSteps) {
        templates.emplace_back(step.pathTemplate);
        if (step.bodyTemplate) {
            templates.emplace_back(*step.bodyTemplate);
        }
        for (const auto& [_, v] : step.headers) {
            templates.emplace_back(v);
        }
    }
    if (actor.refresh) {
        templates.emplace_back(actor.refresh->pathTemplate);
        if (actor.refresh->bodyTemplate) {
            templates.emplace_back(*actor.refresh->bodyTemplate);
        }
        for (const auto& [_, v] : actor.refresh->headers) {
            templates.emplace_back(v);
        }
    }
    return templates;
}

ReqloomError withSource(const SourceLocation& at, ReqloomError error) {
    if (!at.file.empty()) {
        error.detail = at.line > 0 ? std::format("{}:{}: {}", at.file, at.line, error.detail)
                                   : std::format("{}: {}", at.file, error.detail);
        if (error.file.empty()) {
            error.file = at.file;
        }
        if (error.line == 0) {
            error.line = at.line;
        }
    }
    return error;
}

/// `error` pointing at `field` of the entity defined at `at` (see `SourceLocation::lineOf`). Sets
/// only the structured position; `withSource` adds the detail prefix once, at the end.
ReqloomError withField(const SourceLocation& at, std::string_view field, ReqloomError error) {
    if (error.line == 0 && !at.file.empty()) {
        error.file = at.file;
        error.line = at.lineOf(field);
    }
    return error;
}

/// `pointAt` error positioning for a template of the operation at `op` labelled `field`: the
/// project's default auth for an inherited one, else the operation's field.
ReqloomError pointAt(const Operation& op,
                     const Project& project,
                     std::string_view field,
                     ReqloomError error) {
    if (field == kDefaultAuthField) {
        return withField(project.defaultAuthSource, "", std::move(error));
    }
    return withField(op.source, field, std::move(error));
}

/// Each template an operation's request, auth, assertions and poll are built from, labelled with
/// the YAML field that holds it (`SourceLocation::fieldLines` keys), in `operationTemplates` order.
struct LabelledTemplate {
    std::string field;
    std::string_view text;
};

std::vector<LabelledTemplate> labelledTemplates(const Operation& op, const Project& project) {
    std::vector<LabelledTemplate> out;
    out.push_back({"path", op.pathTemplate});
    if (op.bodyTemplate) {
        out.push_back({"body", *op.bodyTemplate});
    }
    for (const auto& [k, v] : op.headers) {
        out.push_back({"headers." + k, v});
    }
    for (const auto& [k, v] : op.queryParams) {
        out.push_back({"query_params." + k, v});
    }
    if (op.bodyForm) {
        for (const auto& [k, v] : *op.bodyForm) {
            out.push_back({"body_form." + k, v});
        }
    }
    // Inherited auth lives in reqloom.yaml, not with the operation (`fieldSource`).
    const bool inherited = op.inlineAuth && op.inlineAuth->type == InlineAuthType::Inherit;
    for (const auto text : authTemplates(op, project)) {
        out.push_back({inherited ? std::string{kDefaultAuthField} : std::string{"auth"}, text});
    }
    for (std::size_t i = 0; i < op.assertions.size(); ++i) {
        out.push_back({std::format("assert[{}]", i), op.assertions[i].expr});
    }
    for (const auto text : pollTemplates(op)) {
        out.push_back({"poll_until", text});
    }
    return out;
}

/// The field of `op` whose template refers to `{{scope.field}}`, or "" when none does.
std::string fieldReferencing(const Operation& op,
                             const Project& project,
                             std::string_view scope,
                             std::string_view field) {
    for (const auto& labelled : labelledTemplates(op, project)) {
        for (const auto& ref : scanReferences({labelled.text})) {
            if (ref.scope == scope && ref.field == field) {
                return labelled.field;
            }
        }
    }
    return {};
}

/// Comma-separated ids of a map's keys, for "known names" hints.
template <typename Map>
std::string joinKeys(const Map& map) {
    std::string out;
    for (const auto& key : map | std::views::keys) {
        out += (out.empty() ? "" : ", ") + key.value;
    }
    return out.empty() ? std::string{"none"} : out;
}

/// Actor ids, resource ids and environment names become file names when the project is saved.
std::expected<void, ReqloomError> validateEntityNames(const Project& project) {
    for (const auto& [id, actor] : project.actors) {
        if (auto ok = checkEntityFileStem("actor id", id.value); !ok) {
            return std::unexpected(withSource(actor.source, ok.error()));
        }
    }
    for (const auto& [id, resource] : project.resources) {
        if (auto ok = checkEntityFileStem("resource id", id.value); !ok) {
            return std::unexpected(withSource(resource.source, ok.error()));
        }
    }
    for (const auto& name : project.environments | std::views::keys) {
        if (auto ok = checkEntityFileStem("environment name", name); !ok) {
            const auto it = project.environmentSources.find(name);
            return std::unexpected(
                withSource(it == project.environmentSources.end() ? SourceLocation{} : it->second,
                           ok.error()));
        }
    }
    return {};
}

/// `$.env` reads in environment values and actor templates obey `allowed_env` like operations.
std::expected<void, ReqloomError> validateEnvReadsOutsideOperations(const Project& project) {
    for (const auto& [envName, vars] : project.environments) {
        const auto it = project.environmentSources.find(envName);
        const auto at = it == project.environmentSources.end() ? SourceLocation{} : it->second;
        for (const auto& [key, value] : vars) {
            const auto owner = std::format("Environment '{}' variable '{}'", envName, key);
            for (const auto& ref : scanReferences({std::string_view{value}})) {
                if (auto ok = checkEnvBuiltinAllowed(owner, ref, project); !ok) {
                    return std::unexpected(withSource(at, ok.error()));
                }
            }
        }
    }
    for (const auto& [id, actor] : project.actors) {
        const auto owner = std::format("Actor '{}'", id.value);
        for (const auto& ref : scanReferences(actorTemplates(actor))) {
            if (auto ok = checkEnvBuiltinAllowed(owner, ref, project); !ok) {
                return std::unexpected(withSource(actor.source, ok.error()));
            }
        }
    }
    return {};
}

/// The faker generators VariableResolver implements; any other name never resolves.
constexpr std::array<std::string_view, 2> kFakerNames{"email", "phone"};

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
    ReqloomError error{
        ErrorCode::RefUndefined,
        ErrorClass::Schema,
        std::format("Operation '{}' uses $.faker.{}, which is not a faker generator; "
                    "valid names: email, phone",
                    opId.value,
                    name)};
    error.reference = std::format("$.faker.{}", name);
    error.fix = "Use {{$.faker.email}} or {{$.faker.phone}}.";
    return std::unexpected(std::move(error));
}

/// Parse every predicate and regex extraction pattern so a malformed one is a load error.
/// The error names the field; `field` receives its `SourceLocation::fieldLines` key.
std::expected<void, ReqloomError> validateOperationExpressions(const OperationId& opId,
                                                               const Operation& op,
                                                               std::string& field) {
    const auto reject =
        [&](const std::string& label, std::string_view expr, const ReqloomError& err) {
            ReqloomError error{ErrorCode::SchemaInvalid,
                               ErrorClass::Schema,
                               std::format("Operation '{}' {}: {}", opId.value, label, err.detail)};
            error.reference = std::string{expr};
            return std::unexpected(std::move(error));
        };
    const auto check = [&](const std::string& label,
                           std::string_view expr) -> std::expected<void, ReqloomError> {
        if (auto ok = validateTemplatedPredicate(expr); !ok) {
            return reject(label, expr, ok.error());
        }
        return {};
    };
    for (std::size_t i = 0; i < op.assertions.size(); ++i) {
        const auto& a = op.assertions[i];
        const auto label =
            a.name ? std::format("assert[{}] '{}'", i, *a.name) : std::format("assert[{}]", i);
        field = std::format("assert[{}]", i);
        if (auto ok = check(label, a.expr); !ok) {
            return ok;
        }
    }
    if (op.pollUntil) {
        field = "poll_until.success_when";
        if (auto ok = check("poll_until.success_when", op.pollUntil->successWhen); !ok) {
            return ok;
        }
        if (op.pollUntil->failWhen) {
            field = "poll_until.fail_when";
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
            field = "extract." + ext.variableName;
            return reject(
                std::format("extract '{}' (regex)", ext.variableName), ext.sourcePath, ok.error());
        }
    }
    field.clear();
    return {};
}

std::expected<void, ReqloomError> validateFailureMessagePaths(
    std::string_view owner, const std::vector<std::string>& paths) {
    for (const auto& path : paths) {
        if (auto ok = validateSingleValueJsonPath(path); !ok) {
            return std::unexpected(
                ReqloomError{ErrorCode::SchemaInvalid,
                             ErrorClass::Schema,
                             std::format("{} failure_message.show: {}", owner, ok.error().detail)});
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

/// Pick the producer(s) of `{{resource.var}}` for `self`: extractors other than `self` that do not
/// also consume it. Several left: those in `self`'s depends_on, else ambiguity. None left: "no
/// producer".
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
        ReqloomError error{
            ErrorCode::RefUndefined,
            ErrorClass::Schema,
            std::format("Operation '{}': no producer for {{{{{}.{}}}}}: every operation that "
                        "extracts it ({}) also consumes it",
                        self.value,
                        refRes,
                        refVar,
                        joinIds(extractors))};
        error.reference = std::format("{{{{{}.{}}}}}", refRes, refVar);
        error.fix = std::format(
            "Make one operation of '{}' extract {} without also referring to it.", refRes, refVar);
        return std::unexpected(std::move(error));
    }
    std::vector<OperationId> named;
    std::ranges::copy_if(candidates, std::back_inserter(named), [&](const OperationId& c) {
        return std::ranges::find(op.explicitDependencies, c) != op.explicitDependencies.end();
    });
    if (!named.empty()) {
        return named;
    }
    ReqloomError error{
        ErrorCode::RefUndefined,
        ErrorClass::Schema,
        std::format("Operation '{}': ambiguous producer for {{{{{}.{}}}}}: candidates {} — "
                    "add depends_on",
                    self.value,
                    refRes,
                    refVar,
                    joinIds(candidates))};
    error.reference = std::format("{{{{{}.{}}}}}", refRes, refVar);
    error.fix = std::format("Add depends_on naming one of: {}.", joinIds(candidates));
    return std::unexpected(std::move(error));
}

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

std::expected<void, ReqloomError> validateOperationTargets(const OperationId& opId,
                                                           const Operation& op,
                                                           const Project& project) {
    const auto missing = [&](std::string_view what,
                             const std::string& id,
                             std::string_view field,
                             std::string known) {
        ReqloomError error{
            ErrorCode::RefUndefined,
            ErrorClass::Schema,
            std::format(
                "Operation '{}' names {} '{}', which is not defined", opId.value, what, id)};
        error.reference = id;
        error.fix = std::format("Name one of the defined ones: {}.", known);
        return std::unexpected(withField(op.source, field, std::move(error)));
    };
    if (!op.actor.value.empty() && !project.actors.contains(op.actor)) {
        return missing("actor", op.actor.value, "actor", joinKeys(project.actors));
    }
    if (op.pollUntil && op.pollUntil->actor && !op.pollUntil->actor->value.empty() &&
        !project.actors.contains(*op.pollUntil->actor)) {
        return missing("poll_until actor",
                       op.pollUntil->actor->value,
                       "poll_until.actor",
                       joinKeys(project.actors));
    }
    if (op.forEach && !project.resources.contains(op.forEach->over)) {
        return missing("for_each resource",
                       op.forEach->over.value,
                       "for_each.over",
                       joinKeys(project.resources));
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

        // Explicit edges first, so an edge both explicit and implied shows as explicit.
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

    // Kahn's sort, lexicographic tie-break; edges mean "current depends on dep", so dependencies
    // come first.
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

    std::ranges::sort(edges, [](const DependencyEdge& a, const DependencyEdge& b) {
        if (a.consumer.value != b.consumer.value) {
            return a.consumer.value < b.consumer.value;
        }
        return a.producer.value < b.producer.value;
    });

    return ResolvedPlan{std::move(sorted), std::move(edges)};
}

namespace {

/// One cycle among the operations Kahn's sort left (in-degree > 0), in dependency order and
/// starting at its alphabetically first member. Operations that only depend on a cycle are left
/// over too; they are peeled off first, so the result names cycle members only.
std::vector<OperationId> findCycle(const std::map<OperationId, std::vector<OperationId>>& graph,
                                   const std::map<OperationId, int>& inDegree) {
    std::set<OperationId> left;
    for (const auto& [node, degree] : inDegree) {
        if (degree > 0) {
            left.insert(node);
        }
    }
    // Peel nodes nothing left depends on: they sit downstream of a cycle, not on one.
    for (bool changed = true; changed;) {
        changed = false;
        for (auto it = left.begin(); it != left.end();) {
            const bool needed = std::ranges::any_of(left, [&](const OperationId& other) {
                const auto deps = graph.find(other);
                return deps != graph.end() &&
                       std::ranges::find(deps->second, *it) != deps->second.end();
            });
            if (needed) {
                ++it;
            } else {
                it = left.erase(it);
                changed = true;
            }
        }
    }
    if (left.empty()) {
        return {};
    }
    // Walk dependencies inside what is left until a node repeats: from there on is a cycle.
    std::vector<OperationId> walk{*left.begin()};
    while (true) {
        const auto deps = graph.find(walk.back());
        std::optional<OperationId> next;
        if (deps != graph.end()) {
            for (const auto& dep : deps->second) {
                if (left.contains(dep)) {
                    next = dep;
                    break;
                }
            }
        }
        if (!next) {
            return walk;
        }
        if (const auto seen = std::ranges::find(walk, *next); seen != walk.end()) {
            std::vector<OperationId> cycle(seen, walk.end());
            std::ranges::rotate(cycle, std::ranges::min_element(cycle));
            return cycle;
        }
        walk.push_back(*next);
    }
}

}  // namespace

std::expected<void, ReqloomError> DependencyResolver::validate(const Project& project) const {
    if (auto names = validateEntityNames(project); !names) {
        return names;
    }
    if (auto envReads = validateEnvReadsOutsideOperations(project); !envReads) {
        return envReads;
    }
    if (auto paths = validateFailureMessagePaths("reqloom.yaml", project.failureMessageShow);
        !paths) {
        return paths;
    }
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

    // Build the whole-project graph so the cycle check sees every edge, not just one target's.
    std::map<OperationId, std::vector<OperationId>> graph;

    for (const auto& [resId, resource] : project.resources) {
        for (const auto& [opName, op] : resource.operations) {
            const auto opId = OperationId{std::format("{}.{}", resId.value, opName)};
            auto checked = [&]() -> std::expected<void, ReqloomError> {
                // A reference whose scope is not a known root can never resolve; `poll_until` may
                // also use `{{response.X}}`.
                for (const auto& labelled : labelledTemplates(op, project)) {
                    const bool poll = labelled.field == "poll_until";
                    for (const auto& ref : scanReferences({labelled.text})) {
                        if (poll && ref.scope == "response") {
                            continue;
                        }
                        if (!isKnownScope(ref.scope)) {
                            ReqloomError error{
                                ErrorCode::RefUndefined,
                                ErrorClass::Schema,
                                std::format("Operation '{}' references undefined symbol '{}.{}': "
                                            "no actor, resource, env, or secret named '{}'",
                                            opId.value,
                                            ref.scope,
                                            ref.field,
                                            ref.scope)};
                            error.reference = std::format("{{{{{}.{}}}}}", ref.scope, ref.field);
                            error.fix = std::format(
                                "Refer to an actor ({}), a resource ({}), env.<key>, "
                                "secret.<NAME> or a $. built-in.",
                                joinKeys(project.actors),
                                joinKeys(project.resources));
                            return std::unexpected(
                                pointAt(op, project, labelled.field, std::move(error)));
                        }
                        if (auto denied = checkEnvBuiltinAllowed(
                                std::format("Operation '{}'", opId.value), ref, project);
                            !denied) {
                            return std::unexpected(
                                pointAt(op, project, labelled.field, denied.error()));
                        }
                        if (auto unknown = checkFakerBuiltinKnown(opId, ref); !unknown) {
                            return std::unexpected(
                                pointAt(op, project, labelled.field, unknown.error()));
                        }
                    }
                }
                std::string expressionField;
                if (auto exprs = validateOperationExpressions(opId, op, expressionField); !exprs) {
                    return std::unexpected(withField(op.source, expressionField, exprs.error()));
                }
                if (op.failureMessageShow) {
                    if (auto paths = validateFailureMessagePaths(
                            std::format("Operation '{}'", opId.value), *op.failureMessageShow);
                        !paths) {
                        return std::unexpected(
                            withField(op.source, "failure_message", paths.error()));
                    }
                }

                std::set<OperationId> allDeps;
                for (std::size_t depIndex = 0; depIndex < op.explicitDependencies.size();
                     ++depIndex) {
                    const auto& dep = op.explicitDependencies[depIndex];
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
                        ReqloomError error{ErrorCode::RefUndefined,
                                           ErrorClass::Schema,
                                           std::format("Operation '{}' declares depends_on '{}', "
                                                       "which is not a defined operation",
                                                       opId.value,
                                                       dep.value)};
                        error.reference = dep.value;
                        error.fix =
                            "Name an existing operation as <resource>.<operation>; "
                            "`reqloom list` shows them.";
                        return std::unexpected(withField(
                            op.source, std::format("depends_on[{}]", depIndex), std::move(error)));
                    }
                    allDeps.insert(dep);
                }

                if (auto targets = validateOperationTargets(opId, op, project); !targets) {
                    return targets;
                }
                const auto implicit = inferImplicitDepsDetailed(op, opId, project);
                if (!implicit) {
                    auto error = implicit.error();
                    // The reference is `{{resource.field}}`; find the field that holds it.
                    std::string field;
                    std::string_view inner{error.reference};
                    if (inner.starts_with("{{") && inner.ends_with("}}")) {
                        inner = inner.substr(2, inner.size() - 4);
                    }
                    if (const auto ref = parseDotted(inner)) {
                        field = fieldReferencing(op, project, ref->scope, ref->field);
                    }
                    return std::unexpected(pointAt(op, project, field, std::move(error)));
                }
                for (const auto& [producer, _] : *implicit) {
                    allDeps.insert(producer);
                }
                graph[opId] = {allDeps.begin(), allDeps.end()};
                return {};
            }();
            if (!checked) {
                return std::unexpected(withSource(op.source, checked.error()));
            }
        }
    }

    // A self-loop never reaches in-degree 0, so it is reported as a cycle.
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
        const auto cycle = findCycle(graph, inDegree);
        std::string cycleOps;
        for (const auto& node : cycle) {
            cycleOps += (cycleOps.empty() ? "" : " → ") + node.value;
        }
        ReqloomError error{
            ErrorCode::Cycle, ErrorClass::Schema, "Circular dependency detected: " + cycleOps};
        error.reference = cycleOps;
        error.fix =
            "Remove one depends_on or {{resource.field}} reference so the operations can "
            "run in an order.";
        // Point at the first operation of the cycle; the detail names them all.
        if (!cycle.empty()) {
            const auto& first = cycle.front();
            const auto dot = first.value.find('.');
            if (const auto res = project.resources.find(ResourceId{first.value.substr(0, dot)});
                dot != std::string::npos && res != project.resources.end()) {
                if (const auto op = res->second.operations.find(first.value.substr(dot + 1));
                    op != res->second.operations.end()) {
                    error = withField(op->second.source, "depends_on", std::move(error));
                }
            }
        }
        return std::unexpected(std::move(error));
    }

    return {};
}

std::expected<void, ReqloomError> checkEntityFileStem(std::string_view kind,
                                                      std::string_view name) {
    // A colon covers Windows drive-letter and alternate-stream forms.
    if (!name.empty() && name.front() != '.' &&
        name.find_first_of(std::string_view{"/\\:\0", 4}) == std::string_view::npos) {
        return {};
    }
    return std::unexpected(ReqloomError{
        ErrorCode::SchemaInvalid,
        ErrorClass::Schema,
        std::format("unsafe {} '{}' (must be non-empty, not start with '.', and contain no '/', "
                    "'\\', ':' or NUL)",
                    kind,
                    name)});
}

std::vector<LabelledReference> operationReferences(const Operation& op, const Project& project) {
    std::vector<LabelledReference> out;
    for (const auto& labelled : labelledTemplates(op, project)) {
        for (auto& ref : scanReferences({labelled.text})) {
            out.push_back(
                LabelledReference{labelled.field, std::move(ref.scope), std::move(ref.field)});
        }
    }
    return out;
}

std::vector<LabelledReference> templateReferences(std::string_view text) {
    std::vector<LabelledReference> out;
    for (auto& ref : scanReferences({text})) {
        out.push_back(LabelledReference{{}, std::move(ref.scope), std::move(ref.field)});
    }
    return out;
}

std::vector<std::string_view> actorReferenceTemplates(const Actor& actor) {
    return actorTemplates(actor);
}

std::vector<std::string> DependencyResolver::collectSecretReferences(const Project& project) {
    std::set<std::string> names;

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

    // Env values can be `!secret NAME` references; scan them so NAME is pre-loaded.
    for (const auto& [_, vars] : project.environments) {
        std::vector<std::string_view> envValues;
        envValues.reserve(vars.size());
        for (const auto& [__, value] : vars) {
            envValues.emplace_back(value);
        }
        scan(envValues);
    }

    for (const auto& [_, actor] : project.actors) {
        scan(actorTemplates(actor));
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

    // Upstream extracts, referenced as `{{R.var}}` where R is the producing op's resource.
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

    const std::string envName = environment.empty() ? project.defaultEnvironment : environment;
    if (const auto envIt = project.environments.find(envName);
        envIt != project.environments.end()) {
        for (const auto& [key, _] : envIt->second) {
            add("env." + key, VariableSuggestion::Kind::EnvVar, envName);
        }
    }

    for (const auto& name : DependencyResolver::collectSecretReferences(project)) {
        add("secret." + name, VariableSuggestion::Kind::Secret, {});
    }

    // Built-ins: function forms get empty parens to fill; `$.now` accepts a +/- duration offset.
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
