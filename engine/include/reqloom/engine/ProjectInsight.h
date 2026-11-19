// ProjectInsight — read-only views of a project for people and agents (PRD FR-13.7–13.9): the
// resolved chain of an operation shaped for display, what one operation consumes and produces and
// who depends on it, and what running a chain would need (sessions, secrets, hosts) without
// sending anything. Shared by the desktop's execution preview and the CLI's `describe` and `plan`.
//
// Nothing here reads a secret value or the keychain, sends a request, asks the person anything or
// writes a file. Templates stay unresolved, so no value a `{{…}}` would produce can leak.
#pragma once

#include <reqloom/engine/Actor.h>
#include <reqloom/engine/Dependency.h>
#include <reqloom/engine/ErrorCodes.h>
#include <reqloom/engine/ExecutionEngine.h>
#include <reqloom/engine/Operation.h>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace reqloom::engine {

class HostApprovalStore;
class RunContext;

/// Uppercase verb of `method` ("GET", "POST", …).
[[nodiscard]] std::string_view httpMethodName(HttpMethod method) noexcept;

/// The schema's name for `strategy` (`simple`, `oauth2_client_credentials`, …).
[[nodiscard]] std::string_view authStrategyName(AuthStrategy strategy) noexcept;

/// One value a step extracts from its response.
struct ChainOutput {
    /// Fully qualified as `<resource>.<name>`, the form a template must use (the schema stores
    /// extraction names bare).
    std::string variable;
    std::string sourcePath;  ///< JSONPath / header name / regex, per the source kind.
};

/// One step of a resolved chain, in execution order.
struct ChainStep {
    int number{0};  ///< 1-based position in the execution order.
    std::string operationId;
    std::string method;        ///< Uppercase verb, empty when the operation is missing.
    std::string pathTemplate;  ///< `{{...}}` left unresolved on purpose.
    std::string actor;
    bool isTarget{false};                ///< The operation asked for: the last step.
    std::vector<std::string> dependsOn;  ///< Producer operation ids, sorted.
    std::vector<ChainOutput> produces;
    std::vector<int> expectStatus;  ///< Declared expected status codes; empty when unset.
};

/// The display model of `plan`, each step resolved against `project`. Steps follow the plan's
/// topological order (target last); a step missing from `project` still appears with only its id.
[[nodiscard]] std::vector<ChainStep> buildChainPreview(const ResolvedPlan& plan,
                                                       const Project& project);

/// Operations in `plan` that consume `variable` produced by `producerOperationId`, sorted.
/// `variable` may be bare or qualified; it is normalised against the producer's resource. Reads the
/// plan's edges only; explicit `depends_on` edges carry no variable and are not counted.
[[nodiscard]] std::vector<std::string> consumersOfVariable(const ResolvedPlan& plan,
                                                           std::string_view producerOperationId,
                                                           std::string_view variable);

/// The operation `id` names (`<resource>.<operation>`, split at the first dot), or null.
[[nodiscard]] const Operation* findOperation(const Project& project, std::string_view id);

/// One `{{…}}` reference an operation makes, and where its value comes from.
struct VariableUse {
    enum class Source : std::uint8_t {
        Extraction,   ///< `{{resource.field}}` extracted by an earlier operation.
        Actor,        ///< `{{actor.field}}` from an actor's session.
        Environment,  ///< `{{env.KEY}}` from the selected environment.
        Secret,       ///< `{{secret.NAME}}` from REQLOOM_SECRET_<NAME> or the keychain.
        ProcessEnv,   ///< `{{$.env.NAME}}` read from the process environment (`allowed_env`).
        Builtin,      ///< `{{$.uuid}}`, `{{$.now}}`, …
        Unknown,      ///< None of the above; load-time validation normally rejects these.
    };

    std::string token;  ///< The reference without braces, e.g. `order.order_id`.
    Source source{Source::Unknown};
    /// The resource, actor, environment variable, secret or process variable it names.
    std::string sourceName;
    /// For `Extraction`: the operations that produce it, as the dependency resolver chose them.
    std::vector<std::string> producers;
    /// For `Environment`: secrets the environment value refers to (`!secret NAME`), names only.
    std::vector<std::string> viaSecrets;
    /// The fields of the operation that use it (`path`, `headers.Authorization`, `assert[0]`, …).
    std::vector<std::string> usedIn;
};

[[nodiscard]] std::string_view variableSourceName(VariableUse::Source source) noexcept;

/// One operation in full.
struct OperationDescription {
    std::string id;
    std::string actor;  ///< Empty when it runs as no actor.
    std::string method;
    std::string pathTemplate;
    SourceLocation source;  ///< Where it is defined (file and line; no field lines).
    std::vector<VariableUse> consumes;
    std::vector<ChainOutput> extracts;
    std::vector<Assertion> assertions;
    std::vector<int> expectStatus;
    std::vector<std::string> dependsOn;   ///< Operations it needs first (direct), sorted.
    std::vector<std::string> dependents;  ///< Operations that need it first (direct), sorted.
};

/// Describe `id`. `environment` (empty: the project default) decides which environment value a
/// `{{env.KEY}}` is looked up in for `viaSecrets`. Errors: `RefUndefined` for an unknown operation,
/// and the resolver's errors for its chain.
[[nodiscard]] std::expected<OperationDescription, ReqloomError> describeOperation(
    const Project& project, std::string_view id, const std::string& environment = {});

/// How a host stands for a credential that a chain would send to it.
enum class HostStatus : std::uint8_t {
    Approved,        ///< Approved for this project and kept in the per-user store.
    ApprovedForRun,  ///< Approved only for the coming run (`REQLOOM_APPROVED_HOSTS`).
    NotApproved,     ///< A run would ask (at a terminal) or fail with E_HOST_NOT_APPROVED.
    Refused,         ///< Outside an actor's allowed_hosts, or the store refuses: a run fails.
    Unknown,         ///< The URL is built from a template, so the host is known only when sent.
};

[[nodiscard]] std::string_view hostStatusName(HostStatus status) noexcept;

/// A host that a chain would send a credential to.
struct PlannedHost {
    std::string host;  ///< Host key (`https://api.example.com:443`), plus a TLS or proxy caveat.
    HostStatus status{HostStatus::Unknown};
    /// What would go there, e.g. "the credentials of actor 'admin'"; never a value.
    std::string credential;
    std::vector<std::string> usedBy;  ///< Operation ids, or `sign-in of actor 'x'`.
    std::string detail;               ///< Why it is refused or unknown; empty otherwise.
};

/// An actor whose session the chain needs.
struct PlannedSession {
    std::string actor;
    std::string strategy;              ///< `simple`, `oauth2_client_credentials`, …
    bool cached{false};                ///< A live session is already in the context: no sign-in.
    std::vector<std::string> secrets;  ///< Secret names its sign-in needs.
};

struct PlannedStep {
    ChainStep step;
    std::vector<std::string> secrets;  ///< Secret names this operation itself needs.
    std::string host;                  ///< Where the request goes, or "" when not literal.
    bool carriesCredential{false};
};

/// What running a chain would need, found without running it.
struct ChainPlan {
    std::string environment;
    std::vector<PlannedStep> steps;
    std::vector<PlannedSession> sessions;
    std::vector<std::string> secrets;  ///< Every secret name the chain needs, sorted.
    std::vector<PlannedHost> hosts;
};

struct PlanOptions {
    std::string environment;  ///< Empty: the project default.
    /// Where approvals are read; never written. Null: every host reads as not approved.
    HostApprovalStore* approvals{nullptr};
    /// Host keys approved for the coming run only (`RunOptions::approvedHostsForRun`).
    std::vector<std::string> approvedForRun;
    /// Sessions a run would start with; null means none (each CLI run starts empty).
    const RunContext* context{nullptr};
};

/// Resolve `target`'s chain and say what a run would need, by the same rules a run applies (the
/// host checks are `HostGate`'s, judged from what each operation refers to). The host of a request
/// is read from the environment's `baseUrl` (or an absolute path) only when it is literal.
[[nodiscard]] std::expected<ChainPlan, ReqloomError> planChain(const Project& project,
                                                               const OperationId& target,
                                                               const PlanOptions& options = {});

/// References to an extracted value that no operation produces (`{{order.owner_id}}` when no
/// operation of `order` extracts `owner_id`): the project loads, but a run fails when it gets
/// there. One finding per operation and reference, with file, line, reference and fix set.
[[nodiscard]] std::vector<ReqloomError> unproducedReferences(const Project& project);

}  // namespace reqloom::engine
