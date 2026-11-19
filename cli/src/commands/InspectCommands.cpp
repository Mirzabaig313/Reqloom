// `reqloom list`, `describe` and `plan` — see header.
#include "InspectCommands.h"

#include "ArgParser.h"
#include "ExitCodes.h"
#include "HostApprovalPrompt.h"
#include "ProjectCommand.h"

#include "../output/JsonWriter.h"
#include "../output/TerminalText.h"

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/HostApproval.h>
#include <reqloom/engine/ProjectInsight.h>
#include <reqloom/engine/PublicApi.h>
#include <reqloom/engine/SecretStore.h>

#include <QtCore/QByteArray>
#include <QtCore/QtEnvironmentVariables>

#include <array>
#include <cstdio>
#include <exception>
#include <format>
#include <print>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace ce = reqloom::engine;

namespace reqloom::cli {

namespace {

constexpr std::array kListOptions{
    OptionSpec{.name = "--project", .takesValue = true},
    OptionSpec{.name = "--format", .takesValue = true},
};

constexpr std::array kOperationOptions{
    OptionSpec{.name = "--project", .takesValue = true},
    OptionSpec{.name = "--env", .takesValue = true},
    OptionSpec{.name = "--format", .takesValue = true},
};

[[nodiscard]] std::string safe(std::string_view text) {
    return terminalSafe(text);
}

[[nodiscard]] std::string joined(const std::vector<std::string>& items,
                                 std::string_view none = "none") {
    if (items.empty()) {
        return std::string{none};
    }
    std::string out;
    for (const auto& item : items) {
        out += (out.empty() ? "" : ", ") + safe(item);
    }
    return out;
}

[[nodiscard]] std::string statusList(const std::vector<int>& statuses) {
    std::string out;
    for (const int status : statuses) {
        out += (out.empty() ? "" : ", ") + std::to_string(status);
    }
    return out.empty() ? std::string{"any 2xx"} : out;
}

[[nodiscard]] std::vector<std::int64_t> toInt64(const std::vector<int>& values) {
    return {values.begin(), values.end()};
}

void numbers(JsonWriter& json, std::string_view name, const std::vector<int>& values) {
    json.key(name).beginArray();
    for (const auto value : toInt64(values)) {
        json.number(value);
    }
    json.endArray();
}

/// Parse options and the single `<resource.operation>` argument for describe and plan.
struct OperationArgs {
    ProjectRequest request;
    std::string operation;
};

[[nodiscard]] std::expected<OperationArgs, std::string> operationArgs(
    std::string command, const ParsedArguments& parsed) {
    auto request = projectRequestFrom(std::move(command), parsed);
    if (!request) {
        return std::unexpected(request.error());
    }
    if (parsed.positionals.empty()) {
        return std::unexpected(std::string{"missing <resource.operation>"});
    }
    if (parsed.positionals.size() > 1) {
        return std::unexpected(std::format("unexpected argument '{}'", parsed.positionals[1]));
    }
    return OperationArgs{std::move(*request), parsed.positionals.front()};
}

[[nodiscard]] std::string operationList(const ce::Project& project) {
    constexpr std::size_t kListed = 50;
    std::string names;
    std::size_t count = 0;
    for (const auto& resource : project.resources | std::views::values) {
        for (const auto& op : resource.operations | std::views::values) {
            if (count++ < kListed) {
                names += (names.empty() ? "" : ", ") + op.id.value;
            }
        }
    }
    if (count > kListed) {
        names += std::format(" and {} more", count - kListed);
    }
    return names.empty() ? std::string{"none"} : names;
}

/// Fail with E_CLI_UNKNOWN_OPERATION unless `id` names an operation of `project`.
[[nodiscard]] std::optional<int> checkOperation(const ProjectRequest& request,
                                                const ce::Project& project,
                                                const std::string& id) {
    if (ce::findOperation(project, id) != nullptr) {
        return std::nullopt;
    }
    return failCommand(
        request,
        kExitUsage,
        kCodeUnknownOperation,
        std::format("unknown operation '{}'; the project has: {}", id, operationList(project)));
}

template <typename Body>
[[nodiscard]] int guarded(const ProjectRequest& request, Body&& body) {
    try {
        return std::forward<Body>(body)();
    } catch (const std::exception& ex) {
        return failCommand(
            request, kExitSystem, kCodeInternal, std::format("unexpected failure: {}", ex.what()));
    }
}

// ─── list ──────────────────────────────────────────────────────────────────────────────────────

void printListUsage(std::FILE* stream) {
    std::print(stream,
               "Usage: reqloom list [options]\n"
               "Every actor, resource and operation in the project, with method, path and the\n"
               "actor each runs as. Nothing is sent.\n"
               "  --project <path>   Project directory (default: cwd)\n"
               "  --format <fmt>     text (default) or json: {{ schema_version, contract_version,\n"
               "                     command, ok, project, default_environment, environments,\n"
               "                     actors: [{{ id, strategy }}], resources: [{{ id, operations:\n"
               "                     [{{ id, method, path, actor, file, line }}] }}] }}\n"
               "Exit codes: 0 listed; 1 the project does not validate; 2 invalid invocation or\n"
               "no reqloom.yaml; 3 output could not be written.\n");
}

[[nodiscard]] std::vector<std::string> environmentNames(const ce::Project& project) {
    std::vector<std::string> names;
    for (const auto& name : project.environments | std::views::keys) {
        names.push_back(name);
    }
    if (std::ranges::find(names, project.defaultEnvironment) == names.end()) {
        names.push_back(project.defaultEnvironment);
    }
    return names;
}

[[nodiscard]] int renderList(const ProjectRequest& request, const ce::Project& project) {
    std::size_t total = 0;
    for (const auto& resource : project.resources | std::views::values) {
        total += resource.operations.size();
    }
    if (!request.json) {
        std::println("{}: {} actors, {} resources, {} operations; environments: {} (default {})",
                     safe(project.name),
                     project.actors.size(),
                     project.resources.size(),
                     total,
                     joined(environmentNames(project)),
                     safe(project.defaultEnvironment));
        std::string actors;
        for (const auto& [id, actor] : project.actors) {
            actors += (actors.empty() ? "" : ", ") +
                      std::format("{} ({})", safe(id.value), ce::authStrategyName(actor.strategy));
        }
        std::println("actors: {}", actors.empty() ? "none" : actors);
        for (const auto& resource : project.resources | std::views::values) {
            std::println("{}", safe(resource.id.value));
            for (const auto& op : resource.operations | std::views::values) {
                std::println("  {:<28} {:<7} {}{}",
                             safe(op.id.value),
                             ce::httpMethodName(op.method),
                             safe(op.pathTemplate),
                             op.actor.value.empty() ? "" : "  as " + safe(op.actor.value));
            }
        }
        return kExitSuccess;
    }
    JsonWriter json;
    beginDocument(json, request.command, true);
    json.field("project", project.name)
        .field("default_environment", project.defaultEnvironment)
        .field("environments", environmentNames(project));
    json.key("actors").beginArray();
    for (const auto& [id, actor] : project.actors) {
        json.beginObject()
            .field("id", id.value)
            .field("strategy", ce::authStrategyName(actor.strategy))
            .endObject();
    }
    json.endArray();
    json.key("resources").beginArray();
    for (const auto& resource : project.resources | std::views::values) {
        json.beginObject().field("id", resource.id.value).key("operations").beginArray();
        for (const auto& op : resource.operations | std::views::values) {
            json.beginObject()
                .field("id", op.id.value)
                .field("method", ce::httpMethodName(op.method))
                .field("path", op.pathTemplate)
                .fieldOrNull("actor", op.actor.value)
                .field("file", projectRelative(project, op.source.file))
                .field("line", op.source.line)
                .endObject();
        }
        json.endArray().endObject();
    }
    json.endArray().endObject();
    return deliverDocument(request.command, json.str(), kExitSuccess);
}

// ─── describe ──────────────────────────────────────────────────────────────────────────────────

void printDescribeUsage(std::FILE* stream) {
    std::print(
        stream,
        "Usage: reqloom describe <resource.operation> [options]\n"
        "One operation in full: actor, method, path, where it is defined, each {{{{…}}}} it\n"
        "consumes and where the value comes from, what it extracts, its expected status and\n"
        "assertions, what it needs first and which operations need it. Templates and names\n"
        "only; no value is resolved and nothing is sent.\n"
        "  --project <path>   Project directory (default: cwd)\n"
        "  --env <name>       Environment whose values are followed to secret names\n"
        "  --format <fmt>     text (default) or json: {{ …, operation: {{ id, actor, method,\n"
        "                     path, file, line, consumes: [{{ token, source, from, producers,\n"
        "                     via_secrets, used_in }}], extracts: [{{ variable, path }}],\n"
        "                     expect_status, assertions: [{{ name, expr }}], depends_on,\n"
        "                     dependents }} }}\n"
        "Exit codes: 0 described; 1 the project does not validate; 2 invalid invocation,\n"
        "unknown operation or environment; 3 output could not be written.\n");
}

[[nodiscard]] std::string useOrigin(const ce::VariableUse& use) {
    using S = ce::VariableUse::Source;
    switch (use.source) {
        case S::Extraction:
            return use.producers.empty()
                       ? std::format("extracted by an operation of '{}' (none does)",
                                     safe(use.sourceName))
                       : "extracted by " + joined(use.producers);
        case S::Actor:
            return std::format("session of actor '{}'", safe(use.sourceName));
        case S::Environment:
            return use.viaSecrets.empty()
                       ? std::format("environment value '{}'", safe(use.sourceName))
                       : std::format("environment value '{}', which is secret {}",
                                     safe(use.sourceName),
                                     joined(use.viaSecrets));
        case S::Secret: {
            const auto variable = ce::secretEnvironmentVariable(use.sourceName);
            return std::format("secret {} ({}the keychain)",
                               safe(use.sourceName),
                               variable ? *variable + " or " : std::string{});
        }
        case S::ProcessEnv:
            return std::format("process environment variable {}", safe(use.sourceName));
        case S::Builtin:
            return "built-in";
        case S::Unknown:
            break;
    }
    return std::format("'{}' is not defined", safe(use.sourceName));
}

[[nodiscard]] int renderDescribe(const ProjectRequest& request,
                                 const ce::Project& project,
                                 const ce::OperationDescription& d) {
    const auto file = projectRelative(project, d.source.file);
    if (!request.json) {
        std::println("{} {} {}", safe(d.id), d.method, safe(d.pathTemplate));
        std::println("  actor: {}", d.actor.empty() ? "none" : safe(d.actor));
        std::println("  defined at: {}:{}", safe(file), d.source.line);
        std::println("  consumes:{}", d.consumes.empty() ? " nothing" : "");
        for (const auto& use : d.consumes) {
            std::println(
                "    {{{{{}}}}}  {}  [{}]", safe(use.token), useOrigin(use), joined(use.usedIn));
        }
        std::println("  extracts:{}", d.extracts.empty() ? " nothing" : "");
        for (const auto& out : d.extracts) {
            std::println("    {{{{{}}}}} <- {}", safe(out.variable), safe(out.sourcePath));
        }
        std::println("  expect_status: {}", statusList(d.expectStatus));
        std::println("  assertions:{}", d.assertions.empty() ? " none" : "");
        for (const auto& a : d.assertions) {
            std::println("    {}{}", a.name ? safe(*a.name) + ": " : "", safe(a.expr));
        }
        std::println("  depends on: {}", joined(d.dependsOn));
        std::println("  needed by: {}", joined(d.dependents));
        return kExitSuccess;
    }
    JsonWriter json;
    beginDocument(json, request.command, true);
    json.key("operation").beginObject();
    json.field("id", d.id)
        .fieldOrNull("actor", d.actor)
        .field("method", d.method)
        .field("path", d.pathTemplate)
        .field("file", file)
        .field("line", d.source.line);
    json.key("consumes").beginArray();
    for (const auto& use : d.consumes) {
        json.beginObject()
            .field("token", use.token)
            .field("source", ce::variableSourceName(use.source))
            .field("from", use.sourceName)
            .field("producers", use.producers)
            .field("via_secrets", use.viaSecrets)
            .field("used_in", use.usedIn)
            .endObject();
    }
    json.endArray();
    json.key("extracts").beginArray();
    for (const auto& out : d.extracts) {
        json.beginObject()
            .field("variable", out.variable)
            .field("path", out.sourcePath)
            .endObject();
    }
    json.endArray();
    numbers(json, "expect_status", d.expectStatus);
    json.key("assertions").beginArray();
    for (const auto& a : d.assertions) {
        json.beginObject()
            .fieldOrNull("name", a.name.value_or(std::string{}))
            .field("expr", a.expr)
            .endObject();
    }
    json.endArray();
    json.field("depends_on", d.dependsOn).field("dependents", d.dependents);
    json.endObject().endObject();
    return deliverDocument(request.command, json.str(), kExitSuccess);
}

// ─── plan ──────────────────────────────────────────────────────────────────────────────────────

void printPlanUsage(std::FILE* stream) {
    std::print(
        stream,
        "Usage: reqloom plan <resource.operation> [options]\n"
        "The chain `reqloom run` would execute, without running it: every step in order and\n"
        "the actor it runs as, the sessions it needs (the CLI keeps none between runs, so\n"
        "each one signs in), the secrets it needs (names only, and whether\n"
        "REQLOOM_SECRET_<NAME> is set), and the hosts that would receive credentials with\n"
        "whether each is approved. Nothing is sent, no secret is read, nobody is asked and\n"
        "nothing is approved.\n"
        "  --project <path>   Project directory (default: cwd)\n"
        "  --env <name>       Environment to plan against (default: the project's)\n"
        "  --format <fmt>     text (default) or json: {{ …, target, environment, ready,\n"
        "                     steps: [{{ order, op, method, path, actor, target, depends_on,\n"
        "                     secrets, carries_credential, host }}], sessions: [{{ actor,\n"
        "                     strategy, cached, secrets }}], secrets: [{{ name, env_variable,\n"
        "                     env_set }}], hosts: [{{ host, status, credential, used_by,\n"
        "                     detail }}], next_action }}\n"
        "Host status: approved, approved_for_run (REQLOOM_APPROVED_HOSTS), not_approved (a\n"
        "run asks at a terminal, else fails with E_HOST_NOT_APPROVED), refused (outside an\n"
        "actor's allowed_hosts; a run fails), unknown (the host comes from a template).\n"
        "Exit codes: 0 planned (also when hosts are not approved); 1 the project does not\n"
        "validate; 2 invalid invocation, unknown operation or environment; 3 output could not\n"
        "be written.\n");
}

struct SecretState {
    std::string name;
    std::string variable;  ///< Empty when no environment variable can carry it.
    bool set{false};
};

[[nodiscard]] std::vector<SecretState> secretStates(const std::vector<std::string>& names) {
    std::vector<SecretState> out;
    for (const auto& name : names) {
        SecretState state{name, {}, false};
        if (const auto variable = ce::secretEnvironmentVariable(name)) {
            state.variable = *variable;
            state.set = qEnvironmentVariableIsSet(variable->c_str());
        }
        out.push_back(std::move(state));
    }
    return out;
}

[[nodiscard]] std::vector<std::string> hostsWith(const ce::ChainPlan& plan, ce::HostStatus status) {
    std::vector<std::string> out;
    for (const auto& host : plan.hosts) {
        if (host.status == status && !host.host.empty()) {
            out.push_back(host.host);
        }
    }
    return out;
}

/// One line of advice built from Reqloom's own data only: it points at fields of the document
/// rather than repeating project text (host names, secret names) in it.
[[nodiscard]] std::string planNextAction(const ce::ChainPlan& plan,
                                         const std::vector<SecretState>& secrets,
                                         std::string_view target) {
    if (!hostsWith(plan, ce::HostStatus::Refused).empty()) {
        return "A run would fail: a host in hosts[] with status refused may not receive the "
               "credentials (its detail says why). Ask the person you work for whether it "
               "should; do not change allowed_hosts or baseUrl to get around this.";
    }
    if (!hostsWith(plan, ce::HostStatus::NotApproved).empty()) {
        return "Each host in hosts[] with status not_approved must be approved before "
               "credentials go there. Ask the person you work for to approve it themselves "
               "(`reqloom hosts approve <host>` at their terminal, or the Reqloom app); do not "
               "approve hosts on their behalf.";
    }
    if (std::ranges::any_of(secrets, [](const SecretState& s) { return !s.set; })) {
        return std::format(
            "Without a keychain (CI, agent sandboxes) set each env_variable in secrets[] whose "
            "env_set is false; then run `reqloom run {}`.",
            target);
    }
    return std::format("Run `reqloom run {}`.", target);
}

[[nodiscard]] bool planReady(const ce::ChainPlan& plan) {
    return std::ranges::none_of(plan.hosts, [](const ce::PlannedHost& h) {
        return h.status == ce::HostStatus::Refused || h.status == ce::HostStatus::NotApproved;
    });
}

void printPlanText(const ce::Project& project,
                   const ce::ChainPlan& plan,
                   const std::vector<SecretState>& secrets,
                   const std::string& target) {
    std::println("Plan for {} in environment {} ({} steps; nothing is sent)",
                 safe(target),
                 safe(plan.environment),
                 plan.steps.size());
    for (const auto& planned : plan.steps) {
        const auto& s = planned.step;
        std::string extra;
        if (!s.actor.empty()) {
            extra += "  as " + safe(s.actor);
        }
        if (!planned.secrets.empty()) {
            extra += "  secrets: " + joined(planned.secrets);
        }
        std::println("  {}. {} {} {}{}{}",
                     s.number,
                     safe(s.operationId),
                     s.method,
                     safe(s.pathTemplate),
                     extra,
                     s.isTarget ? "  <- target" : "");
    }
    std::println("Sessions (the CLI keeps none between runs):{}",
                 plan.sessions.empty() ? " none needed" : "");
    for (const auto& session : plan.sessions) {
        std::println("  {} ({}): {}{}",
                     safe(session.actor),
                     safe(session.strategy),
                     session.cached ? "cached" : "signs in",
                     session.secrets.empty() ? "" : "; secrets: " + joined(session.secrets));
    }
    std::println("Secrets (names only):{}", secrets.empty() ? " none" : "");
    for (const auto& secret : secrets) {
        std::println("  {}  {}",
                     safe(secret.name),
                     secret.variable.empty()
                         ? std::string{"keychain only (no environment variable can carry it)"}
                         : std::format("{} {}", secret.variable, secret.set ? "set" : "not set"));
    }
    std::println("Hosts receiving credentials:{}", plan.hosts.empty() ? " none" : "");
    for (const auto& host : plan.hosts) {
        std::println("  {}  {}  ({}: {})",
                     host.host.empty() ? std::string{"(from a template)"} : safe(host.host),
                     ce::hostStatusName(host.status),
                     safe(host.credential),
                     joined(host.usedBy));
        if (!host.detail.empty()) {
            std::println("    {}", safe(host.detail));
        }
    }
    std::println("Next: {}", safe(planNextAction(plan, secrets, target)));
    (void)project;
}

[[nodiscard]] int renderPlan(const ProjectRequest& request,
                             const ce::Project& project,
                             const ce::ChainPlan& plan,
                             const std::string& target) {
    const auto secrets = secretStates(plan.secrets);
    if (!request.json) {
        printPlanText(project, plan, secrets, target);
        return kExitSuccess;
    }
    JsonWriter json;
    beginDocument(json, request.command, true);
    json.field("target", target)
        .field("environment", plan.environment)
        .field("ready", planReady(plan));
    json.key("steps").beginArray();
    for (const auto& planned : plan.steps) {
        const auto& s = planned.step;
        json.beginObject()
            .field("order", s.number)
            .field("op", s.operationId)
            .field("method", s.method)
            .field("path", s.pathTemplate)
            .fieldOrNull("actor", s.actor)
            .field("target", s.isTarget)
            .field("depends_on", s.dependsOn)
            .field("secrets", planned.secrets)
            .field("carries_credential", planned.carriesCredential)
            .fieldOrNull("host", planned.host)
            .endObject();
    }
    json.endArray();
    json.key("sessions").beginArray();
    for (const auto& session : plan.sessions) {
        json.beginObject()
            .field("actor", session.actor)
            .field("strategy", session.strategy)
            .field("cached", session.cached)
            .field("secrets", session.secrets)
            .endObject();
    }
    json.endArray();
    json.key("secrets").beginArray();
    for (const auto& secret : secrets) {
        json.beginObject()
            .field("name", secret.name)
            .fieldOrNull("env_variable", secret.variable)
            .field("env_set", secret.set)
            .endObject();
    }
    json.endArray();
    json.key("hosts").beginArray();
    for (const auto& host : plan.hosts) {
        json.beginObject()
            .fieldOrNull("host", host.host)
            .field("status", ce::hostStatusName(host.status))
            .field("credential", host.credential)
            .field("used_by", host.usedBy)
            .fieldOrNull("detail", host.detail)
            .endObject();
    }
    json.endArray();
    json.field("next_action", planNextAction(plan, secrets, target));
    json.endObject();
    return deliverDocument(request.command, json.str(), kExitSuccess);
}

template <std::size_t N>
[[nodiscard]] std::expected<OperationArgs, int> parseOperationCommand(
    const std::string& command,
    const QStringList& args,
    const std::array<OptionSpec, N>& options,
    void (*usage)(std::FILE*)) {
    const auto parsed = parseArguments(args, options);
    if (parsed && parsed->help) {
        usage(stdout);
        return std::unexpected(kExitSuccess);
    }
    auto request = parsed
                       ? operationArgs(command, *parsed)
                       : std::expected<OperationArgs, std::string>{std::unexpect, parsed.error()};
    if (!request) {
        return std::unexpected(usageFailure(command, args, request.error()));
    }
    return std::move(*request);
}

}  // namespace

int listCommand(const QStringList& args) {
    const auto parsed = parseArguments(args, kListOptions);
    if (parsed && parsed->help) {
        printListUsage(stdout);
        return kExitSuccess;
    }
    if (parsed && !parsed->positionals.empty()) {
        return usageFailure(
            "list", args, std::format("unexpected argument '{}'", parsed->positionals.front()));
    }
    auto request = parsed
                       ? projectRequestFrom("list", *parsed)
                       : std::expected<ProjectRequest, std::string>{std::unexpect, parsed.error()};
    if (!request) {
        return usageFailure("list", args, request.error());
    }
    return guarded(*request, [&] {
        auto project = loadProjectFor(*request);
        if (!project) {
            return project.error();
        }
        return renderList(*request, *project);
    });
}

int describeCommand(const QStringList& args) {
    auto parsed = parseOperationCommand("describe", args, kOperationOptions, printDescribeUsage);
    if (!parsed) {
        return parsed.error();
    }
    const auto& [request, operation] = *parsed;
    return guarded(request, [&] {
        auto project = loadProjectFor(request);
        if (!project) {
            return project.error();
        }
        if (const auto failed = checkOperation(request, *project, operation)) {
            return *failed;
        }
        const auto description = ce::describeOperation(*project, operation, request.environment);
        if (!description) {
            const auto& error = description.error();
            return failCommand(
                request, kExitFailed, ce::toCodeString(error.code), error.detail, &error);
        }
        return renderDescribe(request, *project, *description);
    });
}

int planCommand(const QStringList& args) {
    auto parsed = parseOperationCommand("plan", args, kOperationOptions, printPlanUsage);
    if (!parsed) {
        return parsed.error();
    }
    const auto& [request, operation] = *parsed;
    return guarded(request, [&] {
        auto project = loadProjectFor(request);
        if (!project) {
            return project.error();
        }
        if (const auto failed = checkOperation(request, *project, operation)) {
            return *failed;
        }
        // Read approvals exactly as `run` would see them, and never write: plan neither asks nor
        // approves. REQLOOM_APPROVED_HOSTS counts only while CLI approval is allowed, as in run.
        const auto approvals = ce::makeFileHostApprovalStore();
        ce::PlanOptions options;
        options.environment = request.environment;
        options.approvals = approvals.get();
        if (approvals->cliApprovalAllowed().value_or(false)) {
            auto approved = hostsApprovedByEnvironment();
            if (!approved) {
                return failCommand(request, kExitUsage, kCodeUsage, approved.error());
            }
            options.approvedForRun = std::move(*approved);
        }
        const auto plan = ce::planChain(*project, ce::OperationId{operation}, options);
        if (!plan) {
            const auto& error = plan.error();
            return failCommand(
                request, kExitFailed, ce::toCodeString(error.code), error.detail, &error);
        }
        return renderPlan(request, *project, *plan, operation);
    });
}

}  // namespace reqloom::cli
