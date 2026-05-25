// Operation — a single endpoint action on a resource (PRD §4.2).
#pragma once

#include <chrono>
#include <compare>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace chainapi::engine {

/// Stable identifier for an operation: "<resource>.<op_name>", e.g. "order.create".
struct OperationId {
    std::string value;

    auto operator<=>(const OperationId&) const = default;
};

/// Stable identifier for a resource, e.g. "order".
struct ResourceId {
    std::string value;

    auto operator<=>(const ResourceId&) const = default;
};

/// Stable identifier for an actor, e.g. "vendor".
struct ActorId {
    std::string value;

    auto operator<=>(const ActorId&) const = default;
};

enum class HttpMethod { Get, Post, Put, Patch, Delete, Head, Options };

/// Where to pull a value out of a response.
struct Extraction {
    enum class Source { JsonPath, XPath, Header, StatusCode, Regex, Cookie };

    std::string variableName;  ///< Stored as <resource>.<variableName>.
    std::string sourcePath;    ///< JSONPath / XPath / header name / regex.
    Source source{Source::JsonPath};
};

/// Per-operation retry policy. Engine spec §3.5.
struct RetryPolicy {
    int maxAttempts{3};
    std::chrono::milliseconds baseBackoff{500};
    std::chrono::milliseconds maxBackoff{30'000};
};

/// Polling configuration. PRD §5.11. Attached to operations whose initial
/// response triggers polling (e.g. 202 Accepted with a status URL).
///
/// Engine semantics:
///   - The initial request runs once. If its response is in
///     `expectStatusList` and a `pollUntil` is set, the polling loop
///     runs until `successWhen` matches, `failWhen` matches, or one of
///     the budgets fires.
///   - Each poll attempt records as its own `StepResult` so the timeline
///     can show predicate evaluations.
///   - Extractions on the parent operation evaluate against the FINAL
///     poll response (whichever response satisfied `successWhen`), not
///     the initial 202.
struct PollUntil {
    HttpMethod method{HttpMethod::Get};

    /// May reference `{{response.headers.Location}}` or
    /// `{{response.body.X}}` from the initial response, in addition
    /// to the standard variable-resolution sources.
    std::string pathTemplate;

    /// Defaults to the parent operation's actor when nullopt.
    std::optional<ActorId> actor;

    /// Predicate against the poll response body (and `$.status_code`).
    /// Required.
    std::string successWhen;

    /// Predicate that short-circuits the poll. Optional but strongly
    /// recommended — without it, a known-bad terminal state turns into
    /// a wall-clock timeout. Wins over `successWhen` if both match a
    /// single response.
    std::optional<std::string> failWhen;

    /// Fixed inter-attempt delay. Mutually exclusive with `backoffBase`.
    std::chrono::milliseconds interval{2'000};

    /// Exponential-backoff base. When set, `interval` is ignored and the
    /// nth attempt waits `min(backoffBase * 2^n, backoffMax)` plus jitter.
    std::optional<std::chrono::milliseconds> backoffBase;
    std::chrono::milliseconds backoffMax{30'000};

    /// Wall-clock cap. Whichever of `timeout` or `maxAttempts` fires
    /// first ends the loop.
    std::chrono::milliseconds timeout{60'000};
    int maxAttempts{30};
};

/// Origin metadata for an operation. PRD §10.3.3.
///
/// Hand-written operations have no provenance. Operations produced by the
/// AI importer carry a populated block so runtime diagnostics (§10.3.4)
/// can cross-reference failures with the original inference, and so the
/// review UI can surface evidence rather than confidence scores.
///
/// Treated as opaque metadata by the runtime — extending the enum or
/// adding fields here must not change execution semantics.
struct Provenance {
    /// How this operation entered the project.
    enum class Source {
        HandWritten,    ///< default; equivalent to "no provenance".
        OpenApiImport,  ///< direct (non-LLM) importer
        PostmanImport,
        BrunoImport,
        InsomniaImport,
        HarImport,
        AiImport,       ///< LLM-driven import (§10)
    };

    /// What kind of sample response (if any) the verifier (§10.3.1) used
    /// to confirm extractions when this op was written.
    enum class VerifiedAgainst {
        None,             ///< verification was not run
        OpenApiExample,
        PostmanResponse,
        InsomniaResponse,
        HarEntry,
        Synthetic,        ///< LLM-produced sample — weakest signal
        LiveCapture,      ///< observed during a real run
    };

    Source source{Source::HandWritten};
    VerifiedAgainst verifiedAgainst{VerifiedAgainst::None};

    /// LLM model id when source is `AiImport` (e.g. "gpt-4o", "claude-sonnet-4.5").
    std::optional<std::string> model;

    /// ISO 8601 wall-clock timestamp at import time.
    std::optional<std::string> importedAt;

    /// Free-form human-readable rationale per inferred field.
    /// Keys are dotted field paths within the operation, e.g.
    /// "actor", "extract.product_id", "depends_on[0]". Values are the
    /// short evidence string surfaced in the review UI.
    std::map<std::string, std::string> evidence;
};

/// One declared operation. 
struct Operation {
    OperationId id;
    ResourceId resource;
    ActorId actor;

    HttpMethod method{HttpMethod::Get};
    std::string pathTemplate;  ///< e.g. /api/v1/orders/{{order.order_id}}
    std::map<std::string, std::string> headers;
    std::map<std::string, std::string> queryParams;
    std::optional<std::string> bodyTemplate;  ///< Raw JSON body template, may contain {{X.y}}.
    std::optional<std::map<std::string, std::string>> bodyForm;  ///< x-www-form-urlencoded

    std::optional<int> expectStatus;

    /// Multi-value form (`expect_status: [200, 202]`). When non-empty,
    /// the executor accepts any code in this list; the singular
    /// `expectStatus` is consulted when the list is empty so existing
    /// schemas keep working unchanged.
    std::vector<int> expectStatusList;

    std::vector<Extraction> extractions;

    /// Explicit dependencies declared by the user 
    std::vector<OperationId> explicitDependencies;

    /// Optional inline JS hook scripts 
    std::optional<std::string> preRequestScript;
    std::optional<std::string> postResponseScript;

    RetryPolicy retry;
    std::optional<std::chrono::milliseconds> timeout;
    bool force{false};  ///< Per-op force re-run flag.

    /// Polling configuration (PRD §5.11). When set, after the initial
    /// request returns a status in `expectStatusList`, the engine
    /// enters a polling loop. The result of `successWhen` against the
    /// final poll response is what flows into extractions.
    std::optional<PollUntil> pollUntil;

    /// Set only for non-hand-written operations. Pure metadata: the
    /// runtime ignores this; tooling and the UI consume it (§10.3.3-4).
    std::optional<Provenance> provenance;
};

}  // namespace chainapi::engine
