// Operation — a single endpoint action on a resource.
#pragma once

#include <chrono>
#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace reqloom::engine {

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

enum class HttpMethod : std::uint8_t { Get, Post, Put, Patch, Delete, Head, Options };

/// Where to pull a value out of a response.
struct Extraction {
    enum class Source : std::uint8_t { JsonPath, XPath, Header, StatusCode, Regex, Cookie };

    std::string variableName;  ///< Stored as <resource>.<variableName>.
    std::string sourcePath;    ///< JSONPath / XPath / header name / regex.
    Source source{Source::JsonPath};
};

/// Per-operation retry policy.
///
/// `maxAttempts` (YAML `retry.max`) is the total number of sends (values below 1 count as 1);
/// waits back off exponentially from `baseBackoff` (`retry.backoff`, ms), capped at
/// `maxBackoff`. Within that budget an attempt is re-sent when:
/// - the transport failed with NetworkDns or NetworkConnect (the request never reached the
///   server): for every method;
/// - another retryable transport error (NetworkTimeout) or an unexpected 5xx occurred: for
///   GET, HEAD, OPTIONS, PUT and DELETE, and for POST and PATCH only when `nonIdempotent`.
/// Anything else (4xx, TLS and policy errors, an expected 5xx) is final.
struct RetryPolicy {
    int maxAttempts{3};
    std::chrono::milliseconds baseBackoff{500};
    std::chrono::milliseconds maxBackoff{30'000};
    /// YAML `retry.non_idempotent: true`: the author declares this POST or PATCH safe to
    /// repeat. No effect on other methods.
    bool nonIdempotent{false};
};

/// Polling for operations that return 202 Accepted: after the initial request matches
/// `expectStatusList` and `pollUntil` is set, the engine polls until `successWhen` or `failWhen`
/// matches or a budget fires. Extractions run against the final poll response.
struct PollUntil {
    HttpMethod method{HttpMethod::Get};

    /// May reference `{{response.headers.Location}}` or
    /// `{{response.body.X}}` from the initial response.
    std::string pathTemplate;

    /// Defaults to the parent operation's actor when nullopt.
    std::optional<ActorId> actor;

    /// Predicate against the poll response body (and `$.status_code`). Required.
    std::string successWhen;

    /// Predicate that short-circuits the poll on a known-bad terminal state.
    /// Wins over `successWhen` if both match the same response.
    std::optional<std::string> failWhen;

    /// Fixed inter-attempt delay. Mutually exclusive with `backoffBase`.
    std::chrono::milliseconds interval{2'000};

    /// Exponential-backoff base. When set, `interval` is ignored and the
    /// nth attempt waits `min(backoffBase * 2^n, backoffMax)`.
    std::optional<std::chrono::milliseconds> backoffBase;
    std::chrono::milliseconds backoffMax{30'000};

    /// Wall-clock cap. Whichever of `timeout` or `maxAttempts` fires
    /// first ends the loop.
    std::chrono::milliseconds timeout{60'000};
    int maxAttempts{30};
};

/// For-each / data-driven execution: the operation runs once per instance of resource `over`;
/// during iteration k, `{{<over>.<field>}}` resolves to its k-th instance.
struct ForEach {
    ResourceId over;

    /// When true a failed iteration does not stop the fan-out; the parent step fails if any failed.
    /// When false (default) the fan-out stops at the first failure.
    bool continueOnError{false};
};

/// Import/verification provenance for non-hand-written operations; opaque to the runtime.
struct Provenance {
    /// How this operation entered the project.
    enum class Source : std::uint8_t {
        HandWritten,    ///< default; equivalent to "no provenance".
        OpenApiImport,  ///< direct (non-LLM) importer
        PostmanImport,
        BrunoImport,
        InsomniaImport,
        HarImport,
        AiImport,  ///< LLM-driven import
        ApidogImport,
        HoppscotchImport,
        ThunderClientImport,
        HttpieImport,
        HttpFileImport,  ///< REST Client / JetBrains `.http` file
        Unknown,         ///< a YAML spelling this version does not know; see `unknownSource`
    };

    /// Kind of sample response (if any) the verifier used to confirm extractions.
    enum class VerifiedAgainst : std::uint8_t {
        None,  ///< verification was not run
        OpenApiExample,
        PostmanResponse,
        InsomniaResponse,
        HarEntry,
        Synthetic,    ///< LLM-produced sample — weakest signal
        LiveCapture,  ///< observed during a real run
    };

    Source source{Source::HandWritten};
    VerifiedAgainst verifiedAgainst{VerifiedAgainst::None};

    /// The original `source:` text when `source` is `Unknown`, written back verbatim so a
    /// newer importer's label survives a save by this version.
    std::string unknownSource;

    std::optional<std::string> model;

    /// ISO 8601 wall-clock timestamp at import time.
    std::optional<std::string> importedAt;

    /// Free-form rationale per inferred field, keyed by dotted field path (e.g.
    /// "extract.product_id"); shown in the review UI.
    std::map<std::string, std::string> evidence;
};

/// Per-operation inline auth kind: a quick credential attached to one endpoint instead of an actor
/// + login chain.
enum class InlineAuthType : std::uint8_t {
    None,      ///< No inline auth (field absent / explicitly cleared).
    Bearer,    ///< Authorization: Bearer <token>
    Basic,     ///< Authorization: Basic base64(username:password)
    ApiKey,    ///< <name>: <value> as a header or query param
    AwsSigV4,  ///< AWS Signature v4, signed per-request (reuses signSigV4Request).
    OAuth1,    ///< OAuth 1.0a HMAC-SHA1, signed per-request (reuses signOAuth1Request).
    OAuth2,    ///< OAuth 2.0 client-credentials; fetches a token, injects Bearer.
    Jwt,       ///< Signs a JWT (HS256/HS512) and injects Authorization: Bearer.
    Mtls,      ///< Mutual TLS — client cert/key for the TLS handshake (transport).
    Inherit,   ///< Use the project's default auth (see Project::defaultAuth).
};

/// A credential bound to a single operation, applied at request-build time. Value fields may
/// contain `{{variable}}` templates; prefer `{{secret.X}}`, since these round-trip verbatim into
/// the YAML.
struct InlineAuth {
    InlineAuthType type{InlineAuthType::None};

    std::string token;     ///< Bearer.
    std::string username;  ///< Basic.
    std::string password;  ///< Basic.

    std::string apiKeyName;     ///< ApiKey: header/query key.
    std::string apiKeyValue;    ///< ApiKey: value.
    bool apiKeyInQuery{false};  ///< ApiKey: true → query param, false → header.

    // AWS Signature v4 — fed to signSigV4Request via a transient session.
    std::string awsAccessKey;
    std::string awsSecretKey;
    std::string awsRegion;
    std::string awsService;
    std::string awsSessionToken;  ///< Optional (STS temporary credentials).

    // OAuth 1.0a (HMAC-SHA1) — fed to signOAuth1Request via a transient session.
    std::string oauthConsumerKey;
    std::string oauthConsumerSecret;
    std::string oauthToken;
    std::string oauthTokenSecret;

    // OAuth 2.0: a token is fetched at request build and injected as Bearer; only non-interactive
    // grants.
    std::string oauth2GrantType;  ///< "client_credentials", "password", or "authorization_code".
    std::string oauth2TokenUrl;
    std::string oauth2ClientId;
    std::string oauth2ClientSecret;
    std::string oauth2Scope;  ///< Optional space-separated scopes.
    std::string
        oauth2ClientAuth;  ///< "body" (default) or "basic" (client creds in a Basic header).
    // Password grant reuses the generic `username`/`password` fields above.

    // Authorization Code (PKCE): interactive; the desktop obtains the token and stores it here.
    std::string oauth2AuthUrl;      ///< Authorization endpoint (browser).
    std::string oauth2CallbackUrl;  ///< Loopback redirect URI.
    std::string oauth2PkceMethod;   ///< "S256" (default) or "plain". Desktop-only.
    std::string oauth2AccessToken;  ///< Token from the interactive flow. NEVER
                                    ///< persisted to YAML (ephemeral secret).

    std::string jwtAlgorithm;  ///< "HS256" (default) or "HS512".
    std::string jwtSecret;
    std::string jwtPayload;  ///< Claims as a JSON object string.

    // Mutual TLS: paths are variable-resolved and passed to the HTTP client as-is, not sandboxed to
    // the project root (client certs often live outside the repo).
    std::string mtlsFormat;       ///< "pem" (default) or "p12" (PKCS#12 bundle).
    std::string mtlsCertPath;     ///< PEM cert, or the .p12/.pfx path when format is p12.
    std::string mtlsKeyPath;      ///< PEM private key (unused for p12).
    std::string mtlsKeyPassword;  ///< Encrypted-key passphrase, or the p12 password.
    std::string mtlsCaCertPath;   ///< Optional custom CA bundle to trust the server.
};

/// A declared response assertion. `expr` uses the predicate grammar shared with
/// `poll_until.success_when`, evaluated against the final response after extractions. `name` is an
/// optional label for reports.
struct Assertion {
    std::string expr;
    std::optional<std::string> name;
};

/// Where an entity was defined: the schema file as the parser opened it and the 1-based line
/// of its key (0 when unknown). Set by the parser, never written to YAML; empty in memory.
struct SourceLocation {
    std::string file;
    int line{0};
    /// 1-based column of the key; 0 when unknown.
    int column{0};
    /// Line of each field inside the entity, keyed by its YAML path relative to the entity:
    /// `path`, `headers`, `headers.Authorization`, `assert[0]`, `extract.token`, … Only fields
    /// present in the file; a lookup falls back to the parent key, then to `line`.
    std::map<std::string, int> fieldLines{};

    /// The line of `field` (see `fieldLines`), else of its nearest parent key, else `line`.
    [[nodiscard]] int lineOf(std::string_view field) const {
        while (!field.empty()) {
            if (const auto it = fieldLines.find(std::string{field}); it != fieldLines.end()) {
                return it->second;
            }
            const auto cut = field.find_last_of(".[");
            field = cut == std::string_view::npos ? std::string_view{} : field.substr(0, cut);
        }
        return line;
    }
};

/// One declared operation.
struct Operation {
    OperationId id;
    ResourceId resource;
    ActorId actor;

    /// Actor-less per-endpoint credential. If both this and `actor` are set, inline auth is applied
    /// last and wins on the Authorization header.
    std::optional<InlineAuth> inlineAuth;

    HttpMethod method{HttpMethod::Get};
    std::string pathTemplate;  ///< e.g. /api/v1/orders/{{order.order_id}}
    std::map<std::string, std::string> headers;
    std::map<std::string, std::string> queryParams;
    std::optional<std::string> bodyTemplate;  ///< Raw JSON body template, may contain {{X.y}}.
    std::optional<std::map<std::string, std::string>> bodyForm;  ///< x-www-form-urlencoded

    std::optional<int> expectStatus;

    /// Multi-value form (`expect_status: [200, 202]`). When non-empty,
    /// takes precedence over the singular `expectStatus`.
    std::vector<int> expectStatusList;

    std::vector<Extraction> extractions;

    /// Declared response assertions, evaluated after extractions against the
    /// final response. Any failing assertion fails the step (AssertionFailed).
    std::vector<Assertion> assertions;

    /// `failure_message.show`: JSONPaths quoted by a status-mismatch failure; replaces
    /// `Project::failureMessageShow`. Empty means the default excerpt; nullopt inherits the project
    /// list.
    std::optional<std::vector<std::string>> failureMessageShow;

    /// Explicit dependencies declared by the user.
    std::vector<OperationId> explicitDependencies;

    /// Optional inline JS hooks (a file reference is loaded into these at parse time).
    std::optional<std::string> preRequestScript;
    std::optional<std::string> postResponseScript;

    /// The original path when a hook came from a file reference (e.g. "./hooks/x.js"); the writer
    /// re-emits the reference instead of inlining so the sidecar file is not orphaned.
    std::optional<std::string> preRequestScriptRef;
    std::optional<std::string> postResponseScriptRef;

    RetryPolicy retry;
    std::optional<std::chrono::milliseconds> timeout;
    bool force{false};  ///< Per-op force re-run flag.

    /// When set, a status in `expectStatusList` after the initial request starts a polling loop;
    /// extractions run against the final poll response.
    std::optional<PollUntil> pollUntil;

    /// When set, runs this operation once per instance of the `over` resource. See `ForEach`.
    std::optional<ForEach> forEach;

    /// Set only for non-hand-written operations; ignored by the runtime.
    std::optional<Provenance> provenance;

    /// Where the operation was defined; see `SourceLocation`.
    SourceLocation source;
};

}  // namespace reqloom::engine
