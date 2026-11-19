// ProjectInsight — see header.
#include <reqloom/engine/ProjectInsight.h>

#include "../domain/DependencyResolver.h"
#include "HookScope.h"
#include "HostGate.h"
#include "StepEvidence.h"

#include <reqloom/engine/Actor.h>
#include <reqloom/engine/HostApproval.h>
#include <reqloom/engine/RunContext.h>

#include <algorithm>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace reqloom::engine {

namespace {

/// Resource half of an operation id ("orders.create" → "orders"); empty if none.
[[nodiscard]] std::string resourceOf(std::string_view operationId) {
    const auto dot = operationId.find('.');
    return dot != std::string_view::npos && dot > 0 ? std::string{operationId.substr(0, dot)}
                                                    : std::string{};
}

/// Namespace an extraction name as a template must reference it: `<producing resource>.<stored
/// key>`. The prefix applies even to dotted keys: `substituteRef` splits at the FIRST dot only, so
/// `employer.org_id` stored in `admin_organization` is referenced as
/// `{{admin_organization.employer.org_id}}`.
[[nodiscard]] std::string qualifyVariable(const std::string& resource,
                                          const std::string& variableName) {
    return resource.empty() ? variableName : resource + "." + variableName;
}

/// Declared expected status codes; the list form wins over the singular field, as in the engine.
[[nodiscard]] std::vector<int> expectedStatuses(const Operation& op) {
    if (!op.expectStatusList.empty()) {
        return op.expectStatusList;
    }
    return op.expectStatus ? std::vector<int>{*op.expectStatus} : std::vector<int>{};
}

[[nodiscard]] std::vector<ChainOutput> outputsOf(const Operation& op, const std::string& opId) {
    std::vector<ChainOutput> out;
    out.reserve(op.extractions.size());
    const auto resource = resourceOf(opId);
    for (const auto& extraction : op.extractions) {
        out.push_back(
            ChainOutput{qualifyVariable(resource, extraction.variableName), extraction.sourcePath});
    }
    return out;
}

template <typename Range>
[[nodiscard]] std::vector<std::string> sortedUnique(const Range& items) {
    std::set<std::string> set(std::ranges::begin(items), std::ranges::end(items));
    return {set.begin(), set.end()};
}

/// Secret names `text` refers to, following `{{env.KEY}}` into the environment value once.
void collectSecrets(std::string_view text,
                    const std::map<std::string, std::string>& envVars,
                    std::set<std::string>& out) {
    for (const auto& ref : templateReferences(text)) {
        if (ref.scope == "secret" && !ref.name.empty()) {
            out.insert(ref.name);
        } else if (ref.scope == "env") {
            if (const auto it = envVars.find(ref.name); it != envVars.end()) {
                for (const auto& inner : templateReferences(it->second)) {
                    if (inner.scope == "secret" && !inner.name.empty()) {
                        out.insert(inner.name);
                    }
                }
            }
        }
    }
}

/// The host key of `url`, or nullopt when its scheme or authority comes from a template (or it
/// does not parse).
[[nodiscard]] std::optional<std::string> literalHostOf(const std::string& url) {
    const auto scheme = url.find("://");
    if (scheme == std::string::npos) {
        return std::nullopt;
    }
    const auto authorityEnd = url.find_first_of("/?#", scheme + 3);
    const auto origin = url.substr(0, authorityEnd);
    if (origin.find("{{") != std::string::npos) {
        return std::nullopt;
    }
    auto key = hostKeyForUrl(origin);
    return key ? std::optional<std::string>{std::move(*key)} : std::nullopt;
}

/// The host of a request the engine builds as `baseUrl` + path (operations, polls, sign-in and
/// refresh steps all do), or nullopt when it comes from a template.
[[nodiscard]] std::optional<std::string> literalHost(const std::string& baseUrl,
                                                     const std::string& pathTemplate) {
    return literalHostOf(baseUrl + pathTemplate);
}

/// The inline auth an operation applies (its own, or the project default for `inherit`).
[[nodiscard]] const InlineAuth* effectiveInlineAuth(const Operation& op, const Project& project) {
    if (!op.inlineAuth) {
        return nullptr;
    }
    if (op.inlineAuth->type == InlineAuthType::Inherit) {
        return project.defaultAuth ? &*project.defaultAuth : nullptr;
    }
    return &*op.inlineAuth;
}

/// A host a credential would go to, gathered over the chain before it is judged.
struct HostUse {
    std::vector<const Actor*> actors;
    std::string credential;
    std::vector<std::string> usedBy;
};

void addHostUse(std::map<std::string, HostUse>& uses,
                const std::string& host,
                const std::vector<const Actor*>& actors,
                const std::string& credential,
                const std::string& usedBy) {
    auto& use = uses[host];
    for (const Actor* actor : actors) {
        if (std::ranges::find(use.actors, actor) == use.actors.end()) {
            use.actors.push_back(actor);
        }
    }
    // Every distinct thing sent there, in first-use order: "the credentials of actor 'a'; the
    // sign-in of actor 'b'".
    if (!credential.empty() && use.credential.find(credential) == std::string::npos) {
        use.credential += (use.credential.empty() ? "" : "; ") + credential;
    }
    if (std::ranges::find(use.usedBy, usedBy) == use.usedBy.end()) {
        use.usedBy.push_back(usedBy);
    }
}

[[nodiscard]] const Actor* actorFor(const Project& project, const std::string& id) {
    if (id.empty()) {
        return nullptr;
    }
    const auto it = project.actors.find(ActorId{id});
    return it == project.actors.end() ? nullptr : &it->second;
}

/// Where an actor's sign-in and refresh requests go: a path after `baseUrl`, or a token URL used
/// as written (OAuth 2 strategies).
struct SignInTarget {
    std::string url;
    bool absolute{false};
};

[[nodiscard]] std::vector<SignInTarget> signInTargets(const Actor& actor) {
    std::vector<SignInTarget> targets;
    switch (actor.strategy) {
        case AuthStrategy::Simple:
        case AuthStrategy::Chain:
            for (const auto& step : actor.authSteps) {
                targets.push_back({step.pathTemplate, false});
            }
            break;
        case AuthStrategy::OAuth2ClientCredentials:
        case AuthStrategy::OAuth2Password:
            if (const auto it = actor.authConfig.find("token_url"); it != actor.authConfig.end()) {
                targets.push_back({it->second, true});
            }
            break;
        default:
            break;
    }
    if (actor.refresh) {
        targets.push_back({actor.refresh->pathTemplate, false});
    }
    return targets;
}

/// Judge one host by `gate`'s rules, never asking: refused by an actor's allowed_hosts or the
/// store, approved (for the run or stored), or not approved.
[[nodiscard]] PlannedHost judgeHost(const HostGate& gate,
                                    const std::string& host,
                                    const HostUse& use,
                                    bool candidateCheck) {
    PlannedHost planned{host, HostStatus::NotApproved, use.credential, use.usedBy, {}};
    if (candidateCheck) {
        const SendPurpose purpose{use.actors, true, use.credential};
        if (auto refused = gate.checkCandidate(host, purpose)) {
            planned.status = HostStatus::Refused;
            planned.detail = refused->detail;
            return planned;
        }
    }
    const auto prior = gate.approvedWithoutAsking(host);
    if (!prior) {
        planned.status = HostStatus::Refused;
        planned.detail = prior.error().detail;
        return planned;
    }
    switch (*prior) {
        case HostGate::PriorApproval::ForRun:
            planned.status = HostStatus::ApprovedForRun;
            break;
        case HostGate::PriorApproval::Stored:
            planned.status = HostStatus::Approved;
            break;
        case HostGate::PriorApproval::None:
            planned.status = HostStatus::NotApproved;
            break;
    }
    return planned;
}

}  // namespace

std::string_view httpMethodName(HttpMethod method) noexcept {
    switch (method) {
        case HttpMethod::Get:
            return "GET";
        case HttpMethod::Post:
            return "POST";
        case HttpMethod::Put:
            return "PUT";
        case HttpMethod::Patch:
            return "PATCH";
        case HttpMethod::Delete:
            return "DELETE";
        case HttpMethod::Head:
            return "HEAD";
        case HttpMethod::Options:
            return "OPTIONS";
    }
    return "GET";
}

std::string_view authStrategyName(AuthStrategy strategy) noexcept {
    switch (strategy) {
        case AuthStrategy::Simple:
            return "simple";
        case AuthStrategy::Chain:
            return "chain";
        case AuthStrategy::Basic:
            return "basic";
        case AuthStrategy::ApiKey:
            return "api_key";
        case AuthStrategy::OAuth2ClientCredentials:
            return "oauth2_client_credentials";
        case AuthStrategy::OAuth2Password:
            return "oauth2_password";
        case AuthStrategy::OAuth1:
            return "oauth1";
        case AuthStrategy::AwsSigV4:
            return "aws_sigv4";
        case AuthStrategy::Bearer:
            return "bearer";
        case AuthStrategy::Jwt:
            return "jwt";
        case AuthStrategy::Mtls:
            return "mtls";
    }
    return "simple";
}

const Operation* findOperation(const Project& project, std::string_view id) {
    const auto dot = id.find('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= id.size()) {
        return nullptr;
    }
    const auto resourceIt = project.resources.find(ResourceId{std::string{id.substr(0, dot)}});
    if (resourceIt == project.resources.end()) {
        return nullptr;
    }
    const auto opIt = resourceIt->second.operations.find(std::string{id.substr(dot + 1)});
    return opIt == resourceIt->second.operations.end() ? nullptr : &opIt->second;
}

std::vector<ChainStep> buildChainPreview(const ResolvedPlan& plan, const Project& project) {
    // Producer ids per consumer, de-duplicated: an explicit depends_on and an implicit `{{var}}`
    // reference are one dependency to a reader.
    std::map<std::string, std::set<std::string>> producers;
    for (const auto& edge : plan.edges) {
        producers[edge.consumer.value].insert(edge.producer.value);
    }

    std::vector<ChainStep> steps;
    steps.reserve(plan.order.size());
    for (std::size_t i = 0; i < plan.order.size(); ++i) {
        const std::string& id = plan.order[i].value;
        ChainStep step;
        step.number = static_cast<int>(i) + 1;
        step.operationId = id;
        step.isTarget = i + 1 == plan.order.size();
        if (const auto it = producers.find(id); it != producers.end()) {
            step.dependsOn.assign(it->second.begin(), it->second.end());
        }
        if (const Operation* op = findOperation(project, id); op != nullptr) {
            step.method = std::string{httpMethodName(op->method)};
            step.pathTemplate = op->pathTemplate;
            step.actor = op->actor.value;
            step.expectStatus = expectedStatuses(*op);
            step.produces = outputsOf(*op, id);
        }
        steps.push_back(std::move(step));
    }
    return steps;
}

std::vector<std::string> consumersOfVariable(const ResolvedPlan& plan,
                                             std::string_view producerOperationId,
                                             std::string_view variable) {
    if (producerOperationId.empty() || variable.empty()) {
        return {};
    }
    // Edges spell the variable "<producing resource>.<stored key>" while extraction events report
    // the stored key, so normalise. Guard on "already starts with this resource's prefix", not
    // "contains a dot": stored keys may contain dots.
    const auto resource = resourceOf(producerOperationId);
    const std::string qualified = resource.empty() || variable.starts_with(resource + ".")
                                      ? std::string{variable}
                                      : resource + "." + std::string{variable};
    std::set<std::string> consumers;
    for (const auto& edge : plan.edges) {
        // Explicit depends_on edges carry no variable, so they aren't evidence this value was
        // consumed.
        if (edge.producer.value == producerOperationId && edge.variable == qualified) {
            consumers.insert(edge.consumer.value);
        }
    }
    return {consumers.begin(), consumers.end()};
}

std::string_view variableSourceName(VariableUse::Source source) noexcept {
    switch (source) {
        case VariableUse::Source::Extraction:
            return "extraction";
        case VariableUse::Source::Actor:
            return "actor_session";
        case VariableUse::Source::Environment:
            return "environment";
        case VariableUse::Source::Secret:
            return "secret";
        case VariableUse::Source::ProcessEnv:
            return "process_environment";
        case VariableUse::Source::Builtin:
            return "builtin";
        case VariableUse::Source::Unknown:
            return "unknown";
    }
    return "unknown";
}

std::string_view hostStatusName(HostStatus status) noexcept {
    switch (status) {
        case HostStatus::Approved:
            return "approved";
        case HostStatus::ApprovedForRun:
            return "approved_for_run";
        case HostStatus::NotApproved:
            return "not_approved";
        case HostStatus::Refused:
            return "refused";
        case HostStatus::Unknown:
            return "unknown";
    }
    return "unknown";
}

std::expected<OperationDescription, ReqloomError> describeOperation(
    const Project& project, std::string_view id, const std::string& environment) {
    const Operation* op = findOperation(project, id);
    if (op == nullptr) {
        return std::unexpected(ReqloomError{ErrorCode::RefUndefined,
                                            ErrorClass::Schema,
                                            std::format("unknown operation '{}'", id)});
    }
    const DependencyResolver resolver;
    const OperationId opId{std::string{id}};
    auto plan = resolver.resolvePlan(project, opId);
    if (!plan) {
        return std::unexpected(plan.error());
    }
    const auto envName = environment.empty() ? project.defaultEnvironment : environment;
    const auto envIt = project.environments.find(envName);
    const std::map<std::string, std::string> noVars;
    const auto& envVars = envIt == project.environments.end() ? noVars : envIt->second;

    OperationDescription out;
    out.id = std::string{id};
    out.actor = op->actor.value;
    out.method = std::string{httpMethodName(op->method)};
    out.pathTemplate = op->pathTemplate;
    out.source = SourceLocation{op->source.file, op->source.line, op->source.column, {}};
    out.extracts = outputsOf(*op, out.id);
    out.assertions = op->assertions;
    out.expectStatus = expectedStatuses(*op);

    std::map<std::string, std::size_t> indexByToken;
    for (const auto& ref : operationReferences(*op, project)) {
        if (ref.field == "poll_until" && ref.scope == "response") {
            continue;  // the poll response itself, not a variable
        }
        const auto token = ref.scope == "$" ? "$." + ref.name : ref.scope + "." + ref.name;
        auto [it, inserted] = indexByToken.try_emplace(token, out.consumes.size());
        if (inserted) {
            VariableUse use;
            use.token = token;
            if (ref.scope == "$") {
                if (ref.name.starts_with("env.")) {
                    use.source = VariableUse::Source::ProcessEnv;
                    use.sourceName = ref.name.substr(4);
                } else {
                    use.source = VariableUse::Source::Builtin;
                    use.sourceName = ref.name;
                }
            } else if (ref.scope == "env") {
                use.source = VariableUse::Source::Environment;
                use.sourceName = ref.name;
                std::set<std::string> secrets;
                if (const auto value = envVars.find(ref.name); value != envVars.end()) {
                    collectSecrets(value->second, envVars, secrets);
                }
                use.viaSecrets.assign(secrets.begin(), secrets.end());
            } else if (ref.scope == "secret") {
                use.source = VariableUse::Source::Secret;
                use.sourceName = ref.name;
            } else if (project.actors.contains(ActorId{ref.scope})) {
                use.source = VariableUse::Source::Actor;
                use.sourceName = ref.scope;
            } else if (project.resources.contains(ResourceId{ref.scope})) {
                use.source = VariableUse::Source::Extraction;
                use.sourceName = ref.scope;
                // A producer named in depends_on as well shows as an explicit edge without the
                // variable, so match the producer's extractions rather than the edge's label.
                std::set<std::string> producers;
                for (const auto& edge : plan->edges) {
                    if (edge.consumer.value != id) {
                        continue;
                    }
                    const Operation* producer = findOperation(project, edge.producer.value);
                    const bool extracts =
                        producer != nullptr && producer->resource.value == ref.scope &&
                        std::ranges::any_of(producer->extractions, [&](const Extraction& e) {
                            return e.variableName == ref.name;
                        });
                    if (edge.variable == token || extracts) {
                        producers.insert(edge.producer.value);
                    }
                }
                use.producers.assign(producers.begin(), producers.end());
            } else {
                use.sourceName = ref.scope;
            }
            out.consumes.push_back(std::move(use));
        }
        auto& usedIn = out.consumes[it->second].usedIn;
        if (std::ranges::find(usedIn, ref.field) == usedIn.end()) {
            usedIn.push_back(ref.field);
        }
    }

    std::set<std::string> dependsOn;
    for (const auto& edge : plan->edges) {
        if (edge.consumer.value == id) {
            dependsOn.insert(edge.producer.value);
        }
    }
    out.dependsOn.assign(dependsOn.begin(), dependsOn.end());

    // Direct dependents: every operation whose own edges name this one as a producer.
    std::set<std::string> dependents;
    for (const auto& resource : project.resources | std::views::values) {
        for (const auto& other : resource.operations | std::views::values) {
            if (other.id.value == id) {
                continue;
            }
            const auto otherPlan = resolver.resolvePlan(project, other.id);
            if (!otherPlan) {
                continue;
            }
            for (const auto& edge : otherPlan->edges) {
                if (edge.consumer == other.id && edge.producer.value == id) {
                    dependents.insert(other.id.value);
                }
            }
        }
    }
    out.dependents.assign(dependents.begin(), dependents.end());
    return out;
}

std::expected<ChainPlan, ReqloomError> planChain(const Project& project,
                                                 const OperationId& target,
                                                 const PlanOptions& options) {
    if (findOperation(project, target.value) == nullptr) {
        return std::unexpected(ReqloomError{ErrorCode::RefUndefined,
                                            ErrorClass::Schema,
                                            std::format("unknown operation '{}'", target.value)});
    }
    auto plan = DependencyResolver{}.resolvePlan(project, target);
    if (!plan) {
        return std::unexpected(plan.error());
    }
    ChainPlan out;
    out.environment =
        options.environment.empty() ? project.defaultEnvironment : options.environment;
    const auto envIt = project.environments.find(out.environment);
    const std::map<std::string, std::string> noVars;
    const auto& envVars = envIt == project.environments.end() ? noVars : envIt->second;
    const auto baseIt = envVars.find("baseUrl");
    const std::string baseUrl = baseIt == envVars.end() ? std::string{} : baseIt->second;
    const auto transportIt = project.transport.find(out.environment);
    const TransportConfig transport =
        transportIt == project.transport.end() ? TransportConfig{} : transportIt->second;
    const auto credentialRefs = credentialReferences(project);

    // The run's own check, configured as `run()` configures it, minus the person to ask.
    HostGate::Config gateConfig{};
    gateConfig.project = &project;
    gateConfig.baseUrl = baseUrl;
    gateConfig.store = options.approvals;
    gateConfig.approvedForRun = options.approvedForRun;
    const HostGate gate{std::move(gateConfig)};

    std::map<std::string, HostUse> hostUses;
    std::vector<std::string> unknownHostUsers;
    std::vector<std::string> sessionOrder;
    std::set<std::string> allSecrets;

    for (auto& chainStep : buildChainPreview(*plan, project)) {
        PlannedStep planned;
        const Operation* op = findOperation(project, chainStep.operationId);
        if (op != nullptr) {
            std::set<std::string> secrets;
            for (const auto& ref : operationReferences(*op, project)) {
                if (ref.scope == "secret" && !ref.name.empty()) {
                    secrets.insert(ref.name);
                } else if (ref.scope == "env") {
                    collectSecrets("{{env." + ref.name + "}}", envVars, secrets);
                }
            }
            for (const auto* script : {&op->preRequestScript, &op->postResponseScript}) {
                if (*script) {
                    secrets.merge(hookScriptSecretNames(**script));
                }
            }
            planned.secrets.assign(secrets.begin(), secrets.end());
            allSecrets.insert(secrets.begin(), secrets.end());

            // Whether the request carries a credential, decided as `guardedSend` decides it.
            std::vector<const Actor*> actors;
            std::string credential;
            if (const Actor* runAs = actorFor(project, op->actor.value)) {
                actors.push_back(runAs);
                credential = "the credentials of actor '" + runAs->id.value + "'";
            }
            const auto use = operationCredentialUse(*op, project, envVars, credentialRefs);
            for (const auto& actorId : use.actors) {
                const Actor* actor = actorFor(project, actorId);
                if (actor != nullptr && std::ranges::find(actors, actor) == actors.end()) {
                    actors.push_back(actor);
                }
            }
            if (credential.empty()) {
                credential = use.reason;
            }
            planned.carriesCredential = !credential.empty();

            // Sessions: the actor it runs as, the poll's actor, and actors whose values it uses.
            std::vector<std::string> sessionActors{op->actor.value};
            if (op->pollUntil && op->pollUntil->actor) {
                sessionActors.push_back(op->pollUntil->actor->value);
            }
            sessionActors.insert(sessionActors.end(), use.actors.begin(), use.actors.end());
            for (const auto& actorId : sessionActors) {
                if (actorFor(project, actorId) != nullptr &&
                    std::ranges::find(sessionOrder, actorId) == sessionOrder.end()) {
                    sessionOrder.push_back(actorId);
                }
            }

            const auto host = literalHost(baseUrl, op->pathTemplate);
            planned.host = host.value_or(std::string{});
            if (planned.carriesCredential) {
                if (host) {
                    addHostUse(hostUses, *host, actors, credential, chainStep.operationId);
                } else {
                    unknownHostUsers.push_back(chainStep.operationId);
                }
            }
            // Inline OAuth 2 fetches its token first, from the token URL as written (the
            // authorization-code grant uses a token the desktop app already holds).
            if (const InlineAuth* auth = effectiveInlineAuth(*op, project);
                auth != nullptr && auth->type == InlineAuthType::OAuth2 &&
                auth->oauth2GrantType != "authorization_code") {
                const auto usedBy = "token request of " + chainStep.operationId;
                if (const auto tokenHost = literalHostOf(auth->oauth2TokenUrl)) {
                    addHostUse(
                        hostUses, *tokenHost, {}, "the inline OAuth 2 client credentials", usedBy);
                } else {
                    unknownHostUsers.push_back(usedBy);
                }
            }
        }
        planned.step = std::move(chainStep);
        out.steps.push_back(std::move(planned));
    }

    for (const auto& actorId : sessionOrder) {
        const Actor& actor = *actorFor(project, actorId);
        PlannedSession session;
        session.actor = actorId;
        session.strategy = std::string{authStrategyName(actor.strategy)};
        if (options.context != nullptr) {
            const auto* live = options.context->session(actor.id);
            session.cached = live != nullptr && live->state == ActorSession::State::Live;
        }
        const auto secrets = actorSecretNames(actor, envVars);
        session.secrets.assign(secrets.begin(), secrets.end());
        if (!session.cached) {
            allSecrets.insert(secrets.begin(), secrets.end());
            const std::string usedBy = "sign-in of actor '" + actorId + "'";
            for (const auto& target : signInTargets(actor)) {
                if (const auto host = target.absolute ? literalHostOf(target.url)
                                                      : literalHost(baseUrl, target.url)) {
                    addHostUse(hostUses,
                               *host,
                               {&actor},
                               "the sign-in of actor '" + actorId + "'",
                               usedBy);
                } else {
                    unknownHostUsers.push_back(usedBy);
                }
            }
        }
        out.sessions.push_back(std::move(session));
    }
    out.secrets.assign(allSecrets.begin(), allSecrets.end());

    for (const auto& [host, use] : hostUses) {
        out.hosts.push_back(judgeHost(gate, host, use, true));
        // Unverified TLS or a project CA bundle is its own approval, as is a proxy (HostGate).
        if (host.starts_with("https://")) {
            std::string caveat;
            if (!transport.tlsVerify || !transport.tlsVerifyHost) {
                caveat = " with TLS verification off";
            } else if (transport.caBundlePath && !transport.caBundlePath->empty()) {
                caveat = " trusting the certificate authority in " + *transport.caBundlePath;
            }
            if (!caveat.empty()) {
                out.hosts.push_back(judgeHost(gate, host + caveat, use, false));
            }
        }
        if (transport.proxy && !transport.proxy->empty()) {
            const auto& proxy = *transport.proxy;
            if (auto proxyHost = hostKeyForUrl(
                    proxy.find("://") == std::string::npos ? "http://" + proxy : proxy)) {
                auto viaProxy = use;
                viaProxy.credential += " (via this proxy)";
                out.hosts.push_back(judgeHost(gate, *proxyHost, viaProxy, false));
            }
        }
    }
    if (!unknownHostUsers.empty()) {
        out.hosts.push_back(PlannedHost{
            {},
            HostStatus::Unknown,
            "a credential",
            sortedUnique(unknownHostUsers),
            "the URL's host comes from a template, so it is known only when the request is "
            "sent; the run checks it then"});
    }
    return out;
}

std::vector<ReqloomError> unproducedReferences(const Project& project) {
    std::vector<ReqloomError> findings;
    for (const auto& resource : project.resources | std::views::values) {
        for (const auto& op : resource.operations | std::views::values) {
            std::set<std::string> reported;
            for (const auto& ref : operationReferences(op, project)) {
                const auto producerIt = project.resources.find(ResourceId{ref.scope});
                if (producerIt == project.resources.end() ||
                    project.actors.contains(ActorId{ref.scope}) || ref.name.empty()) {
                    continue;
                }
                // `{{order[0].id}}` indexes an instance; the field after the index is the name.
                const auto& producerOps = producerIt->second.operations;
                const bool produced = std::ranges::any_of(producerOps, [&](const auto& entry) {
                    return std::ranges::any_of(entry.second.extractions, [&](const Extraction& e) {
                        return e.variableName == ref.name;
                    });
                });
                const auto token = std::format("{{{{{}.{}}}}}", ref.scope, ref.name);
                if (produced || !reported.insert(token).second) {
                    continue;
                }
                std::string extracted;
                for (const auto& entry : producerOps | std::views::values) {
                    for (const auto& e : entry.extractions) {
                        extracted += (extracted.empty() ? "" : ", ") + e.variableName;
                    }
                }
                ReqloomError finding{
                    ErrorCode::VarUnresolved,
                    ErrorClass::Resolution,
                    std::format("Operation '{}' refers to {}, but no operation of '{}' extracts "
                                "'{}'; a run fails when it gets there",
                                op.id.value,
                                token,
                                ref.scope,
                                ref.name)};
                const bool inherited = ref.field == kDefaultAuthField;
                finding.file = inherited ? project.defaultAuthSource.file : op.source.file;
                finding.line =
                    inherited ? project.defaultAuthSource.line : op.source.lineOf(ref.field);
                finding.reference = token;
                finding.fix = std::format(
                    "Add `extract: {{ {}: <JSONPath> }}` to an operation of '{}' (it extracts: "
                    "{}), or correct the name.",
                    ref.name,
                    ref.scope,
                    extracted.empty() ? "nothing" : extracted);
                findings.push_back(std::move(finding));
            }
        }
    }
    return findings;
}

}  // namespace reqloom::engine
