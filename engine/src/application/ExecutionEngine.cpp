// ExecutionEngine — resolves dependency chains, authenticates actors, executes steps.

#include <reqloom/engine/ExecutionEngine.h>

#include "../domain/Codecs.h"
#include "../domain/DependencyResolver.h"
#include "../domain/VariableResolver.h"
#include "../infrastructure/hooks/HookRunner.h"
#include "../infrastructure/http/HttpClient.h"
#include "../infrastructure/schema/SchemaParser.h"
#include "../infrastructure/secrets/SecretStore.h"
#include "../infrastructure/storage/HistoryStore.h"
#include "../infrastructure/util/Crypto.h"
#include "AuthStrategy.h"
#include "Cookies.h"
#include "HeaderMasking.h"
#include "JsonExtraction.h"

#include <reqloom/engine/JsonValues.h>
#include "MultipartBuilder.h"
#include "PredicateEvaluator.h"
#include "RequestSigners.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace reqloom::engine {

using json = nlohmann::json;

namespace {

using namespace codecs;

[[nodiscard]] std::string safeDiagnosticText(const std::string_view raw) {
    constexpr std::size_t kMaxDiagnosticInputBytes{128};
    constexpr std::string_view kHexDigits{"0123456789ABCDEF"};
    std::string safe{};
    safe.reserve(std::min(raw.size(), kMaxDiagnosticInputBytes));
    std::size_t processed{};
    for (const char character : raw) {
        if (processed == kMaxDiagnosticInputBytes) {
            safe += "...";
            break;
        }
        ++processed;
        const auto byte = static_cast<unsigned char>(character);
        if (byte >= 0x20U && byte <= 0x7EU && character != '\\' && character != '"') {
            safe.push_back(character);
            continue;
        }
        safe += "\\x";
        safe.push_back(kHexDigits[byte >> 4U]);
        safe.push_back(kHexDigits[byte & 0x0FU]);
    }
    return safe;
}

[[nodiscard]] bool headerNameEquals(const std::string_view left,
                                    const std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    const auto lowerAscii = [](const unsigned char character) {
        return character >= 'A' && character <= 'Z'
                   ? static_cast<unsigned char>(character + ('a' - 'A'))
                   : character;
    };
    return std::ranges::equal(left, right, [&](const char lhs, const char rhs) {
        return lowerAscii(static_cast<unsigned char>(lhs)) ==
               lowerAscii(static_cast<unsigned char>(rhs));
    });
}

template <typename Value>
void eraseHeaderCaseInsensitive(std::map<std::string, Value>& headers,
                                const std::string_view name) {
    std::erase_if(headers, [&](const auto& entry) { return headerNameEquals(entry.first, name); });
}

template <typename Value>
[[nodiscard]] bool containsHeaderCaseInsensitive(const std::map<std::string, Value>& headers,
                                                 const std::string_view name) {
    return std::ranges::any_of(
        headers, [&](const auto& entry) { return headerNameEquals(entry.first, name); });
}

[[nodiscard]] std::optional<UnresolvedVariableCause> extractionCause(
    const ExtractionTrace::Outcome outcome) noexcept {
    switch (outcome) {
        case ExtractionTrace::Outcome::Resolved:
            return std::nullopt;
        case ExtractionTrace::Outcome::Null:
            return UnresolvedVariableCause::ExtractionNull;
        case ExtractionTrace::Outcome::Missing:
            return UnresolvedVariableCause::ExtractionMissing;
        case ExtractionTrace::Outcome::InvalidPattern:
            return UnresolvedVariableCause::ExtractionInvalid;
        case ExtractionTrace::Outcome::Unsupported:
            return UnresolvedVariableCause::ExtractionUnsupported;
    }
    return std::nullopt;
}

[[nodiscard]] std::string_view trimReferenceBody(std::string_view body) noexcept {
    while (!body.empty() && std::isspace(static_cast<unsigned char>(body.front())) != 0) {
        body.remove_prefix(1);
    }
    while (!body.empty() && std::isspace(static_cast<unsigned char>(body.back())) != 0) {
        body.remove_suffix(1);
    }
    return body;
}

[[nodiscard]] bool hasClosedReference(const std::string_view output,
                                      const std::string_view token) noexcept {
    std::size_t offset{};
    for (auto start = output.find("{{", offset); start != std::string_view::npos;
         start = output.find("{{", offset)) {
        const auto end = output.find("}}", start + 2);
        if (end == std::string_view::npos) {
            return false;
        }
        if (trimReferenceBody(output.substr(start + 2, end - start - 2)) == token) {
            return true;
        }
        offset = end + 2;
    }
    return false;
}

[[nodiscard]] bool isIdentifierLikeReference(const std::string_view token) noexcept {
    const auto dot = token.find('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == token.size()) {
        return false;
    }
    return std::ranges::all_of(token, [](const char character) {
        const auto byte = static_cast<unsigned char>(character);
        return std::isalnum(byte) != 0 || character == '_' || character == '-' ||
               character == '.' || character == '[' || character == ']' || character == '\r' ||
               character == '\n';
    });
}

[[nodiscard]] bool canExposeDiagnosticToken(const std::string_view output,
                                            const std::string_view token) noexcept {
    return isIdentifierLikeReference(token) && hasClosedReference(output, token);
}

struct VariableParts {
    std::string_view rawScope;
    std::string_view sourceScope;
    std::string_view field;
};

[[nodiscard]] std::optional<VariableParts> parseVariableParts(const std::string_view token) {
    const auto dot = token.find('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == token.size()) {
        return std::nullopt;
    }
    const std::string_view rawScope{token.data(), dot};
    auto sourceScope = rawScope;
    if (const auto bracket = sourceScope.find('['); bracket != std::string_view::npos) {
        sourceScope = sourceScope.substr(0, bracket);
    }
    return VariableParts{rawScope, sourceScope, token.substr(dot + 1)};
}

struct ExtractionEvidence {
    UnresolvedVariableCause cause{UnresolvedVariableCause::ResourceValueMissing};
    OperationId producerOp{};
    std::size_t producerStepIndex{};
};

[[nodiscard]] std::optional<std::size_t> priorStepIndex(const OperationId& operation,
                                                        const std::vector<OperationId>& chain,
                                                        const std::size_t currentStepIndex) {
    for (std::size_t index{}; index < currentStepIndex && index < chain.size(); ++index) {
        if (chain[index] == operation) {
            return index;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<ExtractionEvidence> uniqueExtractionEvidence(
    const std::string_view sourceScope,
    const std::string_view field,
    const RunContext& ctx,
    const std::size_t traceStart,
    const std::vector<OperationId>& chain,
    const std::size_t currentStepIndex) {
    std::optional<ExtractionEvidence> evidence{};
    bool matched{};
    const auto& traces = ctx.extractionTrace();
    for (std::size_t index = std::min(traceStart, traces.size()); index < traces.size(); ++index) {
        const auto& trace = traces[index];
        const auto producerDot = trace.op.value.find('.');
        if (producerDot == std::string::npos ||
            std::string_view{trace.op.value}.substr(0, producerDot) != sourceScope ||
            trace.variableName != field) {
            continue;
        }
        if (matched) {
            return std::nullopt;
        }
        matched = true;
        const auto cause = extractionCause(trace.outcome);
        const auto stepIndex = priorStepIndex(trace.op, chain, currentStepIndex);
        if (cause && stepIndex) {
            evidence = ExtractionEvidence{*cause, trace.op, *stepIndex};
        }
    }
    return evidence;
}

[[nodiscard]] UnresolvedVariableDiagnostic makeUnresolvedDiagnostic(
    const VariableResolver::UnresolvedOccurrence& occurrence,
    const bool exposeToken,
    const VariableUseKind useKind,
    const std::string_view useName,
    const std::string_view envName,
    const Project& project,
    const RunContext& ctx,
    const std::size_t traceStart,
    const std::vector<OperationId>& chain,
    const std::size_t currentStepIndex) {
    UnresolvedVariableDiagnostic diagnostic{};
    diagnostic.token = exposeToken ? safeDiagnosticText(occurrence.token) : "<redacted>";
    diagnostic.useKind = useKind;
    diagnostic.useName = safeDiagnosticText(useName);
    const auto parts = exposeToken ? parseVariableParts(occurrence.token) : std::nullopt;
    if (!parts) {
        return diagnostic;
    }
    diagnostic.sourceId = safeDiagnosticText(parts->sourceScope);
    diagnostic.sourceField = safeDiagnosticText(parts->field);
    if (parts->rawScope == "env") {
        diagnostic.cause = UnresolvedVariableCause::EnvironmentValueMissing;
        diagnostic.sourceKind = VariableSourceKind::Environment;
        diagnostic.sourceId = safeDiagnosticText(envName);
    } else if (parts->rawScope == "secret") {
        diagnostic.cause = UnresolvedVariableCause::SecretValueMissing;
        diagnostic.sourceKind = VariableSourceKind::Secret;
    } else if (project.actors.contains(ActorId{std::string{parts->rawScope}})) {
        diagnostic.cause = UnresolvedVariableCause::ActorSessionFieldMissing;
        diagnostic.sourceKind = VariableSourceKind::Actor;
    } else if (project.resources.contains(ResourceId{std::string{parts->sourceScope}})) {
        diagnostic.cause = UnresolvedVariableCause::ResourceValueMissing;
        diagnostic.sourceKind = VariableSourceKind::Resource;
        if (const auto evidence = uniqueExtractionEvidence(
                parts->sourceScope, parts->field, ctx, traceStart, chain, currentStepIndex)) {
            diagnostic.cause = evidence->cause;
            diagnostic.sourceKind = VariableSourceKind::Extraction;
            diagnostic.producerOp = evidence->producerOp;
            diagnostic.producerStepIndex = evidence->producerStepIndex;
        }
    }
    return diagnostic;
}

[[nodiscard]] VariableUseKind useKindForComponent(
    const VariableResolver::Component component) noexcept {
    switch (component) {
        case VariableResolver::Component::UrlPath:
            return VariableUseKind::UrlPath;
        case VariableResolver::Component::RawQuery:
            return VariableUseKind::RawQuery;
        case VariableResolver::Component::Fragment:
            return VariableUseKind::Fragment;
        case VariableResolver::Component::Value:
            return VariableUseKind::Unknown;
    }
    return VariableUseKind::Unknown;
}

[[nodiscard]] std::string_view locationForUseKind(const VariableUseKind useKind) noexcept {
    switch (useKind) {
        case VariableUseKind::UrlPath:
            return "URL path";
        case VariableUseKind::RawQuery:
            return "Raw query";
        case VariableUseKind::Fragment:
            return "URL fragment";
        default:
            return "Request field";
    }
}

struct AuthTemplateUse {
    std::string_view value;
    VariableUseKind useKind{VariableUseKind::Auth};
    std::string_view useName;
    std::string_view location;
};

[[nodiscard]] std::vector<AuthTemplateUse> oauth2TemplateUses(const InlineAuth& auth) {
    if (auth.oauth2GrantType == "authorization_code") {
        return {{auth.oauth2AccessToken,
                 VariableUseKind::Auth,
                 "Authorization",
                 "Authorization header"}};
    }
    std::vector<AuthTemplateUse> uses{
        {auth.oauth2TokenUrl, VariableUseKind::Auth, "OAuth 2 token URL", "Inline auth"},
        {auth.oauth2ClientId, VariableUseKind::Auth, "OAuth 2 client ID", "Inline auth"},
        {auth.oauth2ClientSecret, VariableUseKind::Auth, "OAuth 2 client secret", "Inline auth"},
        {auth.oauth2Scope, VariableUseKind::Auth, "OAuth 2 scope", "Inline auth"},
    };
    if (auth.oauth2GrantType == "password") {

        uses.push_back({auth.username, VariableUseKind::Auth, "OAuth 2 username", "Inline auth"});
        uses.push_back({auth.password, VariableUseKind::Auth, "OAuth 2 password", "Inline auth"});
    }
    return uses;
}

[[nodiscard]] std::vector<AuthTemplateUse> mtlsTemplateUses(const InlineAuth& auth) {
    std::vector<AuthTemplateUse> uses{
        {auth.mtlsCertPath, VariableUseKind::Auth, "mTLS certificate", "Inline auth"},
        {auth.mtlsKeyPassword, VariableUseKind::Auth, "mTLS key password", "Inline auth"},
        {auth.mtlsCaCertPath, VariableUseKind::Auth, "mTLS CA certificate", "Inline auth"},
    };
    if (auth.mtlsFormat != "p12") {
        uses.push_back(
            {auth.mtlsKeyPath, VariableUseKind::Auth, "mTLS private key", "Inline auth"});
    }
    return uses;
}

[[nodiscard]] std::vector<AuthTemplateUse> inlineAuthTemplateUses(const InlineAuth& auth) {
    constexpr auto kAuth = VariableUseKind::Auth;
    switch (auth.type) {
        case InlineAuthType::Bearer:
            return {{auth.token, kAuth, "Authorization", "Authorization header"}};
        case InlineAuthType::Basic:
            return {{auth.username, kAuth, "Authorization", "Authorization header"},
                    {auth.password, kAuth, "Authorization", "Authorization header"}};
        case InlineAuthType::ApiKey:
            return {{auth.apiKeyName, kAuth, "API key", "Inline auth"},
                    {auth.apiKeyValue, kAuth, "API key", "Inline auth"}};
        case InlineAuthType::AwsSigV4:
            return {{auth.awsAccessKey, kAuth, "Authorization", "Authorization header"},
                    {auth.awsSecretKey, kAuth, "Authorization", "Authorization header"},
                    {auth.awsRegion, kAuth, "Authorization", "Authorization header"},
                    {auth.awsService, kAuth, "Authorization", "Authorization header"},
                    {auth.awsSessionToken, kAuth, "Authorization", "Authorization header"}};
        case InlineAuthType::OAuth1:
            return {{auth.oauthConsumerKey, kAuth, "Authorization", "Authorization header"},
                    {auth.oauthConsumerSecret, kAuth, "Authorization", "Authorization header"},
                    {auth.oauthToken, kAuth, "Authorization", "Authorization header"},
                    {auth.oauthTokenSecret, kAuth, "Authorization", "Authorization header"}};
        case InlineAuthType::OAuth2:
            return oauth2TemplateUses(auth);
        case InlineAuthType::Jwt:
            return {{auth.jwtSecret, kAuth, "Authorization", "Authorization header"},
                    {auth.jwtPayload, kAuth, "Authorization", "Authorization header"}};
        case InlineAuthType::Mtls:
            return mtlsTemplateUses(auth);
        case InlineAuthType::None:
        case InlineAuthType::Inherit:
            return {};
    }
    return {};
}

/// Overlay an actor session's mTLS transport (client cert/key/CA) onto a
/// request, composing with — not replacing — the environment transport already
/// on `req`. Only non-empty fields win, so an mTLS actor adds its client cert
/// while keeping the environment's proxy/timeout settings.
void applyActorSessionTransport(HttpRequest& req, const ActorSession& session) {
    if (!session.transport) {
        return;
    }
    const auto& t = *session.transport;
    if (t.clientCertPath.has_value()) {
        req.transport.clientCertPath = t.clientCertPath;
    }
    if (t.clientCertType.has_value()) {
        req.transport.clientCertType = t.clientCertType;
    }
    if (t.clientKeyPath.has_value()) {
        req.transport.clientKeyPath = t.clientKeyPath;
    }
    if (t.clientKeyPassword.has_value()) {
        req.transport.clientKeyPassword = t.clientKeyPassword;
    }
    if (t.caBundlePath.has_value()) {
        req.transport.caBundlePath = t.caBundlePath;
    }
}

/// Build a HookContext snapshot from current run state.
/// hooks get read-only access to actor variables; we copy them so the
/// hook can't reach back into RunContext via reference.
[[nodiscard]] HookContext buildHookContext(const HttpRequest& req,
                                           const RunContext& ctx,
                                           const ResolveContext& rctx,
                                           const Project& project) {
    HookContext out;
    out.request.method = req.method;
    out.request.url = req.url;
    out.request.headers = req.headers;
    out.request.body = req.body;

    for (const auto& [actorId, _] : project.actors) {
        if (const auto* sess = ctx.session(actorId); sess) {
            out.variables[actorId.value] = sess->variables;
        }
    }
    out.env = rctx.envVars;
    out.secrets = rctx.secrets;
    return out;
}

/// Convert HttpResponse's vector<pair> headers (curl preserves order
/// and casing) into the map<string,string> the hook surface expects.
[[nodiscard]] std::map<std::string, std::string> headersToMap(
    const std::vector<std::pair<std::string, std::string>>& headers) {
    std::map<std::string, std::string> out;
    for (const auto& [k, v] : headers) {
        out[k] = v;
    }
    return out;
}

/// Convert a hook-mutated header map back into vector<pair> form so the
/// downstream pipeline (extraction, response viewer) sees the same shape
/// it would have seen without the hook.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> headersToVector(
    const std::map<std::string, std::string>& headers) {
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(headers.size());
    for (const auto& [k, v] : headers) {
        out.emplace_back(k, v);
    }
    return out;
}

/// Bytes the request body puts on the wire — inline body, or the sum of
/// multipart part sizes.
[[nodiscard]] std::size_t requestBodySize(const HttpRequest& req) noexcept {
    if (!req.multipart.empty()) {
        std::size_t total = 0;
        for (const auto& part : req.multipart) {
            total += part.value.size();  // text fields + pre-loaded file bytes
        }
        return total;
    }
    return req.body ? req.body->size() : 0U;
}

void appendQueryString(std::string& url, std::string_view query) {
    if (query.empty()) {
        return;
    }
    const auto fragmentStart = url.find('#');
    const auto queryStart = url.find('?');
    const bool hasQuery = queryStart != std::string::npos &&
                          (fragmentStart == std::string::npos || queryStart < fragmentStart);
    const std::string addition = std::string{hasQuery ? "&" : "?"} + std::string{query};
    url.insert(fragmentStart == std::string::npos ? url.size() : fragmentStart, addition);
}

void rewriteQueryParameter(std::string& url,
                           const std::string_view encodedKey,
                           const std::string_view encodedValue) {
    const auto fragmentStart = url.find('#');
    const auto queryStart = url.find('?');
    if (queryStart == std::string::npos ||
        (fragmentStart != std::string::npos && queryStart > fragmentStart)) {
        appendQueryString(url, std::string{encodedKey} + "=" + std::string{encodedValue});
        return;
    }
    const auto queryEnd = fragmentStart == std::string::npos ? url.size() : fragmentStart;
    const std::string_view query{url.data() + queryStart + 1, queryEnd - queryStart - 1};
    const auto decodedKey = urlDecode(encodedKey);
    std::string rebuilt{};
    for (std::size_t start{}; start <= query.size();) {
        const auto end = query.find('&', start);
        const auto segment = query.substr(start, end == std::string_view::npos ? end : end - start);
        const auto segmentKey = segment.substr(0, segment.find('='));
        const auto decodedSegmentKey = urlDecode(segmentKey);
        const bool matchesKey = segmentKey == encodedKey || (decodedKey && decodedSegmentKey &&
                                                             *decodedSegmentKey == *decodedKey);
        if (!segment.empty() && !matchesKey) {
            rebuilt += std::string{rebuilt.empty() ? "" : "&"} + std::string{segment};
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    rebuilt += std::string{rebuilt.empty() ? "" : "&"} + std::string{encodedKey} + "=" +
               std::string{encodedValue};
    url.replace(queryStart + 1, queryEnd - queryStart - 1, rebuilt);
}

void setQueryParameter(std::string& url, const std::string_view key, const std::string_view value) {
    const auto encodedKey = urlEncode(key);
    const auto encodedValue = urlEncode(value);
    rewriteQueryParameter(url, encodedKey, encodedValue);
}

[[nodiscard]] std::string sanitizeEventUrl(const std::string_view url) {
    const std::string redacted = urlEncode("<redacted>");
    const auto fragmentStart = url.find('#');
    const auto queryCandidate = url.find('?');
    const auto queryStart =
        queryCandidate != std::string_view::npos &&
                (fragmentStart == std::string_view::npos || queryCandidate < fragmentStart)
            ? queryCandidate
            : std::string_view::npos;
    const auto pathEnd =
        std::min(queryStart == std::string_view::npos ? url.size() : queryStart,
                 fragmentStart == std::string_view::npos ? url.size() : fragmentStart);

    std::string sanitized{};
    std::size_t pathStart{};
    const auto schemeEnd = url.find("://");
    const bool hasAuthority =
        schemeEnd != std::string_view::npos && schemeEnd < pathEnd && schemeEnd > 0 &&
        std::isalpha(static_cast<unsigned char>(url.front())) != 0 &&
        std::ranges::all_of(url.substr(1, schemeEnd - 1), [](const char c) {
            const auto byte = static_cast<unsigned char>(c);
            return std::isalnum(byte) != 0 || c == '+' || c == '-' || c == '.';
        });
    if (hasAuthority) {
        const auto authorityStart = schemeEnd + 3;
        const auto authorityEnd = url.find_first_of("/?#", authorityStart);
        pathStart = authorityEnd == std::string_view::npos ? url.size() : authorityEnd;
        sanitized.append(url.substr(0, authorityStart));
        sanitized += redacted;
    }

    if (pathEnd > pathStart) {
        const auto path = url.substr(pathStart, pathEnd - pathStart);
        if (path == "/") {
            sanitized += '/';
        } else {
            if (path.starts_with('/')) {
                sanitized += '/';
            }
            sanitized += redacted;
        }
    }

    if (queryStart != std::string_view::npos) {
        sanitized += '?';
        const auto queryEnd = fragmentStart == std::string_view::npos ? url.size() : fragmentStart;
        const auto query = url.substr(queryStart + 1, queryEnd - queryStart - 1);
        for (std::size_t start{}; start <= query.size();) {
            const auto end = query.find('&', start);
            const auto segment = query.substr(
                start, end == std::string_view::npos ? query.size() - start : end - start);
            if (start != 0) {
                sanitized += '&';
            }
            if (!segment.empty()) {
                sanitized += redacted;
            }
            if (end == std::string_view::npos) {
                break;
            }
            start = end + 1;
        }
    }

    if (fragmentStart != std::string_view::npos) {
        sanitized += '#';
        if (fragmentStart + 1 < url.size()) {
            sanitized += redacted;
        }
    }
    return sanitized;
}

}  // namespace

struct ExecutionEngine::Impl {
    Dependencies deps;
    DependencyResolver resolver;
    VariableResolver varResolver;
    std::vector<EventCallback> subscribers;
    std::mutex subscriberMutex;
    std::atomic<std::uint64_t> nextRunId{1};
    // 0 = nothing cancelled; any other value = the run with that id is being cancelled.
    std::atomic<std::uint64_t> cancelledRunId{0};

    // Set per-run from RunOptions. Runs are serialized on one engine
    // instance (concurrent run() is unsupported in the MVP), so a plain
    // member is safe — no run overlaps another's read of this flag.
    bool captureResponseBodies{false};

    explicit Impl(Dependencies d) : deps(std::move(d)) {}

    [[nodiscard]] bool isCancelled(RunId runId) const noexcept {
        const auto cancelled = cancelledRunId.load(std::memory_order_acquire);
        return cancelled != 0 && cancelled == runId.value;
    }

    /// Build the opt-in body payload for a `ResponseReceived` event.
    /// Returns nullopt unless the run opted in, capping the captured
    /// bytes at `kMaxCapturedBodyBytes` (UTF-8-aware so the in-memory
    /// string a viewer renders never ends mid-character) to bound
    /// event/UI memory.
    [[nodiscard]] std::optional<std::string> capturedBody(const std::string& body) const {
        if (!captureResponseBodies) {
            return std::nullopt;
        }
        if (body.size() > kMaxCapturedBodyBytes) {
            return codecs::truncateUtf8(body, kMaxCapturedBodyBytes);
        }
        return body;
    }

    /// Walk a response's Set-Cookie headers and update the actor's jar.
    /// Order-preserving: when the same name appears twice in the same
    /// response, the second one wins (RFC 6265 §5.3 step 11). The jar
    /// is shared across operations performed AS this actor for the
    /// remainder of the run.
    static void absorbResponseCookies(
        const std::vector<std::pair<std::string, std::string>>& headers,
        const ActorId& actor,
        RunContext& ctx) {
        const auto fresh = cookies::collectFromResponse(headers);
        for (const auto& [name, value] : fresh) {
            ctx.setCookie(actor, name, value);
        }
    }

    void emit(const RunEvent& event) {
        RunEvent safeEvent{event};
        if (auto* prepared = std::get_if<RequestPrepared>(&safeEvent); prepared != nullptr) {
            // A prepared URL is display evidence, not a replayable request. Treat
            // authority, path, query segments, and fragment as opaque because
            // templates and hooks can source any of them from secrets.
            prepared->url = sanitizeEventUrl(prepared->url);
        }
        // Persist before fanning out — the event survives a subscriber
        // that crashes the process. Best-effort: a persistence failure
        // must never break the run. append() returns errors via
        // std::expected, but a serialization edge (e.g. nlohmann dump on
        // unexpected input) could still throw; swallow it here so a
        // history hiccup can never abort the chain or escape across the
        // engine boundary on the worker thread.
        if (deps.history) {
            try {
                // NOLINTNEXTLINE(bugprone-unused-return-value)
                (void)deps.history->append(safeEvent);
                // NOLINTNEXTLINE(bugprone-empty-catch)
            } catch (...) {
            }
        }

        // Snapshot before invoking — avoids re-entrant deadlock if a callback calls subscribe().
        std::vector<EventCallback> snapshot;
        {
            const std::lock_guard lock(subscriberMutex);
            snapshot = subscribers;
        }
        for (auto& cb : snapshot) {
            try {
                cb(safeEvent);
                // Subscriber isolation is intentional: a misbehaving callback
                // must not propagate into the engine's run loop, which would
                // break the chain for every other subscriber. Once the
                // engine logger lands (Engine Requirement §10), this becomes
                // log + continue.
                // NOLINTNEXTLINE(bugprone-empty-catch)
            } catch (...) {
            }
        }
    }

    // Authenticate an actor if session is not live. Returns true on success.

    bool ensureSession(const Actor& actor,
                       RunContext& ctx,
                       const ResolveContext& rctx,
                       RunId runId,
                       std::size_t stepIndex) {
        // Forwards auth-flow events into Impl::emit so they reach
        // subscribers and history under the parent step's runId/stepIndex.
        auto sink = [this](const RunEvent& ev) {
            this->emit(ev);
        };

        const auto* existing = ctx.session(actor.id);
        if ((existing != nullptr) && existing->state == ActorSession::State::Live) {
            const auto now = std::chrono::steady_clock::now();
            if (now < existing->expiresAt) {
                return true;
            }

            if (actor.refresh) {
                AuthDependencies const refreshDeps{
                    deps.http.get(), &varResolver, sink, runId, stepIndex, captureResponseBodies};
                auto refreshed = runRefresh(actor, ctx, rctx, refreshDeps);
                if (refreshed) {
                    ActorSession updated = *existing;
                    for (auto& [k, v] : *refreshed) {
                        updated.variables[k] = std::move(v);
                    }
                    updated.state = ActorSession::State::Live;
                    updated.expiresAt = std::chrono::steady_clock::now() + actor.sessionTtl;
                    ctx.putSession(actor.id, std::move(updated));
                    emit(SessionRefreshed{runId,
                                          actor.id,
                                          SessionRefreshed::Trigger::Expiry,
                                          std::chrono::system_clock::now()});
                    return true;
                }
                // Refresh failed — fall through to full re-auth.
            }
        }

        AuthDependencies authDeps{
            deps.http.get(), &varResolver, sink, runId, stepIndex, captureResponseBodies};
        auto authenticator = selectAuthenticator(actor, std::move(authDeps));
        if (!authenticator) {
            return false;
        }

        auto outcome = authenticator->authenticate(actor, ctx, rctx);
        if (!outcome) {
            return false;
        }

        ActorSession session = std::move(*outcome);
        session.state = ActorSession::State::Live;
        session.expiresAt = std::chrono::steady_clock::now() + actor.sessionTtl;
        ctx.putSession(actor.id, std::move(session));
        return true;
    }

    // Run the polling phase for an operation. Returns the FINAL poll response on
    // success. Errors: PollFailPredicate, PollTimeout, PollMaxAttemptsExceeded,
    // SchemaInvalid. Cancellation is checked each iteration.
    std::expected<HttpResponse, ReqloomError> runPollLoop(const Operation& op,
                                                          const PollUntil& poll,
                                                          const Project& project,
                                                          RunContext& ctx,
                                                          const ResolveContext& rctx,
                                                          RunId runId,
                                                          std::size_t stepIndex,
                                                          const HttpResponse& /*initialResponse*/,
                                                          std::vector<StepResult>& attemptRows) {
        PredicateEvaluator const evaluator;

        auto successPredicate = evaluator.parse(poll.successWhen);
        if (!successPredicate) {
            return std::unexpected(
                ReqloomError{ErrorCode::SchemaInvalid,
                             ErrorClass::Schema,
                             "poll_until.success_when: " + successPredicate.error().detail});
        }

        std::optional<ParsedPredicate> failPredicate;
        if (poll.failWhen) {
            auto parsed = evaluator.parse(*poll.failWhen);
            if (!parsed) {
                return std::unexpected(
                    ReqloomError{ErrorCode::SchemaInvalid,
                                 ErrorClass::Schema,
                                 "poll_until.fail_when: " + parsed.error().detail});
            }
            failPredicate = std::move(*parsed);
        }

        const auto baseUrlIt = rctx.envVars.find("baseUrl");
        const std::string baseUrl = baseUrlIt != rctx.envVars.end() ? baseUrlIt->second : "";

        const Actor* pollActor = nullptr;
        if (poll.actor) {
            auto it = project.actors.find(*poll.actor);
            if (it != project.actors.end()) {
                pollActor = &it->second;
            }
        } else if (!op.actor.value.empty()) {
            auto it = project.actors.find(op.actor);
            if (it != project.actors.end()) {
                pollActor = &it->second;
            }
        }

        if ((pollActor != nullptr) && !ensureSession(*pollActor, ctx, rctx, runId, stepIndex)) {
            return std::unexpected(ReqloomError{ErrorCode::SessionRefreshFailed,
                                                ErrorClass::Auth,
                                                "poll_until: actor session refresh failed"});
        }

        const auto deadline = std::chrono::steady_clock::now() + poll.timeout;
        HttpResponse lastResponse;
        bool haveLastResponse = false;

        // Inline auth for the poll requests, computed once. OAuth2 fetches its
        // token a single time here (not per poll attempt) and the resulting
        // Bearer header is re-applied to every attempt.
        const std::optional<InlineAuth> effAuth = effectiveInlineAuth(op, project);
        std::map<std::string, std::string> inlineOAuth2Headers;
        if (effAuth && effAuth->type == InlineAuthType::OAuth2) {
            HttpRequest scratch;
            if (auto oauth2Err = applyInlineOAuth2(scratch, effAuth, ctx, rctx, runId, stepIndex)) {
                return std::unexpected(*oauth2Err);
            }
            inlineOAuth2Headers = std::move(scratch.headers);
        }

        for (int attempt = 0; attempt < poll.maxAttempts; ++attempt) {
            if (isCancelled(runId)) {
                return std::unexpected(
                    ReqloomError{ErrorCode::Cancelled, ErrorClass::Run, "poll_until: cancelled"});
            }

            HttpRequest req;
            req.method = poll.method;
            req.transport = rctx.transport;
            applyInlineMtls(req, effAuth, ctx, rctx);
            auto resolvedPath = varResolver.resolveUrlPath(poll.pathTemplate, ctx, rctx);
            if (!resolvedPath.unresolved.empty()) {
                return std::unexpected(ReqloomError{
                    ErrorCode::VarUnresolved,
                    ErrorClass::Resolution,
                    "poll_until: unresolved variable in path: " + resolvedPath.unresolved.front()});
            }
            req.url = baseUrl + resolvedPath.output;
            if (pollActor != nullptr) {
                for (const auto& [k, v] : pollActor->inject.headers) {
                    auto resolved = varResolver.resolve(v, ctx, rctx);
                    if (!resolved.unresolvedOccurrences.empty()) {
                        const auto& occurrence = resolved.unresolvedOccurrences.front();
                        const auto token =
                            canExposeDiagnosticToken(resolved.output, occurrence.token)
                                ? safeDiagnosticText(occurrence.token)
                                : std::string{"<redacted>"};
                        return std::unexpected(ReqloomError{
                            ErrorCode::VarUnresolved,
                            ErrorClass::Resolution,
                            "poll_until: unresolved variable in actor header: " + token});
                    }
                    eraseHeaderCaseInsensitive(req.headers, k);
                    req.headers[k] = resolved.output;
                }
                // Session-level inject. Session wins on key collision.
                if (const auto* session = ctx.session(pollActor->id); session) {
                    for (const auto& [k, v] : session->injectHeaders) {
                        eraseHeaderCaseInsensitive(req.headers, k);
                        req.headers[k] = v;
                    }
                    applyActorSessionTransport(req, *session);
                    if (!session->injectQueryParams.empty()) {
                        std::string qs;
                        for (const auto& [k, v] : session->injectQueryParams) {
                            if (!qs.empty()) {
                                qs += "&";
                            }
                            qs += urlEncode(k) + "=" + urlEncode(v);
                        }
                        appendQueryString(req.url, qs);
                    }
                }

                // Cookie jar emission for poll requests. Same priority
                // as the parent op: a poll inheriting the parent's
                // actor sees the jar that has accumulated through the
                // initial response and any prior poll attempt.
                if (!containsHeaderCaseInsensitive(req.headers, "Cookie")) {
                    const auto jar = ctx.cookies(pollActor->id);
                    if (!jar.empty()) {
                        req.headers["Cookie"] = cookies::formatRequestHeader(jar);
                    }
                }
            }

            // Inline (actor-less) auth on the parent op also covers its poll
            // requests. Applied before signing; inline-auth ops have no signing
            // session, so the two never interact in practice.
            if (auto authErr = applyInlineAuth(req, effAuth, ctx, rctx)) {
                return std::unexpected(*authErr);
            }
            for (const auto& [key, value] : inlineOAuth2Headers) {
                eraseHeaderCaseInsensitive(req.headers, key);
                req.headers[key] = value;
            }

            // Per-request signing done after inject merge so the signer sees the final shape.
            if (pollActor != nullptr) {
                if (const auto* session = ctx.session(pollActor->id); session) {
                    if (session->signingScheme == ActorSession::SigningScheme::OAuth1HmacSha1) {
                        if (!signOAuth1Request(req, *session)) {
                            return std::unexpected(
                                ReqloomError{ErrorCode::SessionRefreshFailed,
                                             ErrorClass::Auth,
                                             "poll_until: oauth1 signing failed (missing "
                                             "consumer credentials or malformed URL)"});
                        }
                    } else if (session->signingScheme == ActorSession::SigningScheme::AwsSigV4) {
                        if (!signSigV4Request(req, *session)) {
                            return std::unexpected(
                                ReqloomError{ErrorCode::SessionRefreshFailed,
                                             ErrorClass::Auth,
                                             "poll_until: aws_sigv4 signing failed "
                                             "(missing access_key/secret_key/region/service "
                                             "or malformed URL)"});
                        }
                    }
                }
            }

            // Inline (actor-less) signing on the parent op also covers polls.
            if (!signInlineAuth(req, effAuth, ctx, rctx)) {
                return std::unexpected(ReqloomError{ErrorCode::SessionRefreshFailed,
                                                    ErrorClass::Auth,
                                                    "poll_until: inline auth signing failed "
                                                    "(missing credentials or malformed URL)"});
            }

            emit(RequestPrepared{runId,
                                 stepIndex,
                                 req.method,
                                 req.url,
                                 headersToVector(maskHeaders(req.headers)),
                                 requestBodySize(req),
                                 std::chrono::system_clock::now()});

            auto resp = deps.http->send(req);
            if (!resp) {
                return std::unexpected(resp.error());
            }
            lastResponse = std::move(*resp);
            haveLastResponse = true;
            emit(ResponseReceived{runId,
                                  stepIndex,
                                  lastResponse.status,
                                  maskHeaders(lastResponse.headers),
                                  lastResponse.body.size(),
                                  lastResponse.elapsed,
                                  std::chrono::system_clock::now(),
                                  capturedBody(lastResponse.body)});

            // Update the cookie jar from this poll's Set-Cookie headers.
            // Pollers that issue stateful status checks (rare but real)
            // can rotate session cookies — without absorbing them here
            // a follow-up op would send a stale cookie.
            if (pollActor != nullptr) {
                absorbResponseCookies(lastResponse.headers, pollActor->id, ctx);
            }

            // Each poll attempt is a timeline row alongside the parent step.
            // Parse the body once per attempt and evaluate both predicates
            // against it — evaluate() would otherwise re-parse the same
            // body for each predicate.
            const auto parsedBody = evaluator.parseBody(lastResponse.body);
            const auto failMatched =
                failPredicate &&
                evaluator.evaluate(*failPredicate, parsedBody, lastResponse.status) ==
                    PredicateValue::True;
            const auto successMatched =
                !failMatched &&
                evaluator.evaluate(*successPredicate, parsedBody, lastResponse.status) ==
                    PredicateValue::True;

            StepResult attemptRow;
            attemptRow.op = op.id;
            attemptRow.pollAttempt = attempt + 1;
            attemptRow.attempts = 1;
            if (failMatched) {
                attemptRow.status = StepResult::Status::Failed;
                attemptRow.error = ErrorCode::PollFailPredicate;
                attemptRow.detail =
                    "fail_when matched (HTTP " + std::to_string(lastResponse.status) + ")";
            } else if (successMatched) {
                attemptRow.status = StepResult::Status::Succeeded;
                attemptRow.detail =
                    "success_when matched (HTTP " + std::to_string(lastResponse.status) + ")";
            } else {
                // Treat in-progress polls as Pending so renderers show "still working".
                attemptRow.status = StepResult::Status::Pending;
                attemptRow.detail =
                    "in_progress (HTTP " + std::to_string(lastResponse.status) + ")";
            }
            ctx.record(attemptRow);
            attemptRows.push_back(std::move(attemptRow));

            if (failMatched) {
                return std::unexpected(ReqloomError{ErrorCode::PollFailPredicate,
                                                    ErrorClass::Polling,
                                                    "poll_until.fail_when matched (HTTP " +
                                                        std::to_string(lastResponse.status) + ")"});
            }
            if (successMatched) {
                return lastResponse;
            }

            // Next-attempt delay: fixed interval or exponential backoff.
            std::chrono::milliseconds delay = poll.interval;
            if (poll.backoffBase) {
                const auto shift = std::min(attempt, 20);
                auto raw = *poll.backoffBase * (std::uint32_t{1} << shift);
                delay = (raw < poll.backoffMax) ? raw : poll.backoffMax;
            }

            // Floor delay to avoid busy-looping on `interval: 0ms`.
            constexpr auto kMinPollDelay = std::chrono::milliseconds{50};
            if (delay < kMinPollDelay) {
                delay = kMinPollDelay;
            }

            const auto remaining = deadline - std::chrono::steady_clock::now();
            if (remaining <= std::chrono::milliseconds{0}) {
                return std::unexpected(ReqloomError{
                    ErrorCode::PollTimeout,
                    ErrorClass::Polling,
                    haveLastResponse ? "poll_until: timeout exceeded — last response: HTTP " +
                                           std::to_string(lastResponse.status)
                                     : "poll_until: timeout exceeded"});
            }
            const auto sleepFor =
                std::min(std::chrono::duration_cast<std::chrono::milliseconds>(remaining), delay);
            std::this_thread::sleep_for(sleepFor);
        }

        return std::unexpected(ReqloomError{
            ErrorCode::PollMaxAttemptsExceeded,
            ErrorClass::Polling,
            haveLastResponse
                ? "poll_until: max_attempts (" + std::to_string(poll.maxAttempts) +
                      ") exceeded — last response: HTTP " + std::to_string(lastResponse.status)
                : "poll_until: max_attempts (" + std::to_string(poll.maxAttempts) + ") exceeded"});
    }

    // Apply an operation's actor-less inline auth to a fully-built request.
    // Values may contain {{variables}}, resolved here like any header. Called
    // after actor/session injects so inline auth wins on the Authorization key.
    [[nodiscard]] std::optional<ReqloomError> applyInlineAuth(HttpRequest& req,
                                                              const std::optional<InlineAuth>& auth,
                                                              const RunContext& ctx,
                                                              const ResolveContext& rctx) const {
        if (!auth || auth->type == InlineAuthType::None) {
            return std::nullopt;
        }
        switch (auth->type) {
            case InlineAuthType::Bearer: {
                const auto token = varResolver.resolve(auth->token, ctx, rctx).output;
                eraseHeaderCaseInsensitive(req.headers, "Authorization");
                req.headers["Authorization"] = "Bearer " + token;
                break;
            }
            case InlineAuthType::Basic: {
                const auto user = varResolver.resolve(auth->username, ctx, rctx).output;
                const auto pass = varResolver.resolve(auth->password, ctx, rctx).output;
                eraseHeaderCaseInsensitive(req.headers, "Authorization");
                req.headers["Authorization"] = "Basic " + base64Encode(user + ":" + pass);
                break;
            }
            case InlineAuthType::ApiKey: {
                const auto key = varResolver.resolve(auth->apiKeyName, ctx, rctx).output;
                if (key.empty()) {
                    return ReqloomError{ErrorCode::SessionRefreshFailed,
                                        ErrorClass::Auth,
                                        "api key: resolved name is empty"};
                }
                const auto value = varResolver.resolve(auth->apiKeyValue, ctx, rctx).output;
                if (auth->apiKeyInQuery) {
                    setQueryParameter(req.url, key, value);
                } else {
                    eraseHeaderCaseInsensitive(req.headers, key);
                    req.headers[key] = value;
                }
                break;
            }
            case InlineAuthType::Jwt: {
                const auto secret = varResolver.resolve(auth->jwtSecret, ctx, rctx).output;
                const auto payload = varResolver.resolve(auth->jwtPayload, ctx, rctx).output;
                // Only HS256/HS512 are supported (crypto has no RS/ES). Empty
                // algorithm defaults to HS256; anything else fails the step
                // rather than silently signing with the wrong algorithm.
                std::string algo = auth->jwtAlgorithm;
                std::ranges::transform(algo, algo.begin(), [](unsigned char c) {
                    return static_cast<char>(std::toupper(c));
                });
                if (algo.empty()) {
                    algo = "HS256";
                }
                if (algo != "HS256" && algo != "HS512") {
                    return ReqloomError{ErrorCode::SessionRefreshFailed,
                                        ErrorClass::Auth,
                                        "jwt: unsupported algorithm '" + auth->jwtAlgorithm +
                                            "' (only HS256 and HS512 are supported)"};
                }
                const std::string jwt = algo == "HS512" ? crypto::jwtSignHs512(payload, secret)
                                                        : crypto::jwtSignHs256(payload, secret);
                if (jwt.empty()) {
                    return ReqloomError{ErrorCode::SessionRefreshFailed,
                                        ErrorClass::Auth,
                                        "jwt: signing failed (empty secret or crypto error)"};
                }
                eraseHeaderCaseInsensitive(req.headers, "Authorization");
                req.headers["Authorization"] = "Bearer " + jwt;
                break;
            }
            case InlineAuthType::None:
            case InlineAuthType::AwsSigV4:
            case InlineAuthType::OAuth1:
            case InlineAuthType::OAuth2:
            case InlineAuthType::Mtls:
            case InlineAuthType::Inherit:
                // AwsSigV4/OAuth1 sign per-attempt (signInlineAuth); OAuth2 is a
                // token fetch (applyInlineOAuth2); Mtls is transport-level;
                // Inherit is resolved to the project default before we get here.
                break;
        }
        return std::nullopt;
    }

    // Per-request signing for inline (actor-less) AWS SigV4 / OAuth1. Reuses
    // the same tested signers as the actor path by feeding the inline
    // credentials into a transient session. Returns false on signing failure
    // (missing credential / malformed URL), leaving req untouched.
    [[nodiscard]] bool signInlineAuth(HttpRequest& req,
                                      const std::optional<InlineAuth>& auth,
                                      const RunContext& ctx,
                                      const ResolveContext& rctx) const {
        if (!auth) {
            return true;
        }
        const auto resolve = [&](const std::string& value) {
            return varResolver.resolve(value, ctx, rctx).output;
        };
        switch (auth->type) {
            case InlineAuthType::AwsSigV4: {
                ActorSession session;
                session.variables["access_key"] = resolve(auth->awsAccessKey);
                session.variables["secret_key"] = resolve(auth->awsSecretKey);
                session.variables["region"] = resolve(auth->awsRegion);
                session.variables["service"] = resolve(auth->awsService);
                if (!auth->awsSessionToken.empty()) {
                    session.variables["session_token"] = resolve(auth->awsSessionToken);
                }
                eraseHeaderCaseInsensitive(req.headers, "Authorization");
                return signSigV4Request(req, session);
            }
            case InlineAuthType::OAuth1: {
                ActorSession session;
                session.variables["consumer_key"] = resolve(auth->oauthConsumerKey);
                session.variables["consumer_secret"] = resolve(auth->oauthConsumerSecret);
                session.variables["token"] = resolve(auth->oauthToken);
                session.variables["token_secret"] = resolve(auth->oauthTokenSecret);
                eraseHeaderCaseInsensitive(req.headers, "Authorization");
                return signOAuth1Request(req, session);
            }
            case InlineAuthType::None:
            case InlineAuthType::Bearer:
            case InlineAuthType::Basic:
            case InlineAuthType::ApiKey:
            case InlineAuthType::OAuth2:
            case InlineAuthType::Jwt:
            case InlineAuthType::Mtls:
            case InlineAuthType::Inherit:
                return true;  // non-signing types handled elsewhere
        }
        return true;
    }

    // Resolve an operation's effective inline auth. Inherit resolves to the
    // project default (which must not itself be Inherit; a nested Inherit or an
    // absent default resolves to no auth).
    [[nodiscard]] std::optional<InlineAuth> effectiveInlineAuth(const Operation& op,
                                                                const Project& project) const {
        if (op.inlineAuth && op.inlineAuth->type == InlineAuthType::Inherit) {
            if (project.defaultAuth && project.defaultAuth->type != InlineAuthType::Inherit) {
                return project.defaultAuth;
            }
            return std::nullopt;
        }
        return op.inlineAuth;
    }

    // Mutual TLS: stamp the client cert/key onto the request's transport so the
    // HTTP client presents them during the handshake. Transport-level, so it
    // composes with (does not replace) any header auth.
    void applyInlineMtls(HttpRequest& req,
                         const std::optional<InlineAuth>& auth,
                         const RunContext& ctx,
                         const ResolveContext& rctx) const {
        if (!auth || auth->type != InlineAuthType::Mtls) {
            return;
        }
        const bool p12 = auth->mtlsFormat == "p12";
        if (!auth->mtlsCertPath.empty()) {
            req.transport.clientCertPath =
                varResolver.resolve(auth->mtlsCertPath, ctx, rctx).output;
        }
        if (p12) {
            // PKCS#12 bundle: the key lives inside the cert; tell curl the type.
            req.transport.clientCertType = "P12";
        } else if (!auth->mtlsKeyPath.empty()) {
            req.transport.clientKeyPath = varResolver.resolve(auth->mtlsKeyPath, ctx, rctx).output;
        }
        if (!auth->mtlsKeyPassword.empty()) {
            req.transport.clientKeyPassword =
                varResolver.resolve(auth->mtlsKeyPassword, ctx, rctx).output;
        }
        if (!auth->mtlsCaCertPath.empty()) {
            req.transport.caBundlePath =
                varResolver.resolve(auth->mtlsCaCertPath, ctx, rctx).output;
        }
    }

    // OAuth 2.0 (client credentials): fetch a token by synthesizing a transient
    // actor and reusing the tested OAuth2 authenticator, then inject the
    // resulting Authorization: Bearer. Returns an error on token-fetch failure.
    //
    // The token is cached in the RunContext session store keyed by
    // token_url|client_id|scope, honoring the token endpoint's `expires_in`
    // (with a small safety margin), so ops sharing the same OAuth2 config —
    // and repeated runs on the same context — reuse one token instead of
    // re-fetching per operation.
    [[nodiscard]] std::optional<ReqloomError> applyInlineOAuth2(
        HttpRequest& req,
        const std::optional<InlineAuth>& auth,
        RunContext& ctx,
        const ResolveContext& rctx,
        RunId runId,
        std::size_t stepIndex) {
        if (!auth || auth->type != InlineAuthType::OAuth2) {
            return std::nullopt;
        }

        // Authorization Code (PKCE) is interactive: the desktop runs the browser
        // flow and stores the token on the op. The engine only injects it — it
        // can't acquire one headlessly.
        if (auth->oauth2GrantType == "authorization_code") {
            const auto token = varResolver.resolve(auth->oauth2AccessToken, ctx, rctx).output;
            if (token.empty()) {
                return ReqloomError{ErrorCode::SessionRefreshFailed,
                                    ErrorClass::Auth,
                                    "oauth2 authorization_code: no access token — use "
                                    "\"Get New Token\" in the desktop app to authorize"};
            }
            eraseHeaderCaseInsensitive(req.headers, "Authorization");
            req.headers["Authorization"] = "Bearer " + token;
            return std::nullopt;
        }

        const ActorId cacheId{"__inline_oauth2:" + auth->oauth2GrantType + "|" +
                              auth->oauth2TokenUrl + "|" + auth->oauth2ClientId + "|" +
                              auth->username + "|" + auth->oauth2Scope};

        // Reuse a still-valid cached token.
        if (const auto* cached = ctx.session(cacheId);
            cached != nullptr && cached->state == ActorSession::State::Live &&
            std::chrono::steady_clock::now() < cached->expiresAt) {
            for (const auto& [key, value] : cached->injectHeaders) {
                eraseHeaderCaseInsensitive(req.headers, key);
                req.headers[key] = value;
            }
            return std::nullopt;
        }

        Actor actor;
        actor.id = cacheId;
        // Only the non-interactive grants are supported headlessly. "password"
        // uses the password grant; anything else uses client_credentials.
        actor.strategy = auth->oauth2GrantType == "password"
                             ? AuthStrategy::OAuth2Password
                             : AuthStrategy::OAuth2ClientCredentials;
        actor.authConfig["token_url"] = auth->oauth2TokenUrl;
        actor.authConfig["client_id"] = auth->oauth2ClientId;
        actor.authConfig["client_secret"] = auth->oauth2ClientSecret;
        actor.authConfig["username"] = auth->username;
        actor.authConfig["password"] = auth->password;
        actor.authConfig["scope"] = auth->oauth2Scope;
        actor.authConfig["client_auth"] = auth->oauth2ClientAuth;

        auto sink = [this](const RunEvent& ev) {
            this->emit(ev);
        };
        AuthDependencies authDeps{
            deps.http.get(), &varResolver, sink, runId, stepIndex, captureResponseBodies};
        auto authenticator = selectAuthenticator(actor, std::move(authDeps));
        if (!authenticator) {
            return ReqloomError{ErrorCode::SessionRefreshFailed,
                                ErrorClass::Auth,
                                "inline oauth2: no authenticator for client_credentials"};
        }
        auto session = authenticator->authenticate(actor, ctx, rctx);
        if (!session) {
            return session.error();
        }
        for (const auto& [key, value] : session->injectHeaders) {
            eraseHeaderCaseInsensitive(req.headers, key);
            req.headers[key] = value;
        }

        // Cache with a lifetime from `expires_in` (default 5 min, minus a 30s
        // safety margin so we never present a token about to expire).
        auto ttl = std::chrono::seconds{300};
        if (const auto it = session->variables.find("expires_in"); it != session->variables.end()) {
            long secs{0};
            const auto& s = it->second;
            if (std::from_chars(s.data(), s.data() + s.size(), secs).ec == std::errc{} &&
                secs > 0) {
                ttl = std::chrono::seconds{secs};
            }
        }
        const auto margin = std::chrono::seconds{30};
        session->state = ActorSession::State::Live;
        session->expiresAt = std::chrono::steady_clock::now() + (ttl > margin ? ttl - margin : ttl);
        ctx.putSession(cacheId, *session);
        return std::nullopt;
    }

    // Execute a single operation step. pollAttemptRows is an out-parameter since
    // each timeline row is a separate StepResult (a parent can't carry a
    // vector<StepResult> without making the type recursive).
    StepResult executeStep(const Operation& op,
                           const Project& project,
                           RunContext& ctx,
                           const ResolveContext& rctx,
                           RunId runId,
                           std::size_t stepIndex,
                           const std::vector<OperationId>& chain,
                           std::size_t extractionTraceStart,
                           std::string_view envName,
                           std::vector<StepResult>& pollAttemptRows,
                           std::vector<UnresolvedVariableDiagnostic>& diagnostics) {
        StepResult result;
        result.op = op.id;
        result.attempts = 1;
        auto startTime = std::chrono::steady_clock::now();

        constexpr std::size_t kMaxDiagnosticsPerStep{128};
        bool foundUnresolved{};
        bool ignoreCurrentActorUnresolved{};
        std::optional<std::pair<std::string, std::string>> firstUnresolved{};

        const auto unresolvedVariableDetail = [](const std::string_view safeVariable,
                                                 const std::string_view location) {
            return std::string{"Variable: {{"} + std::string{safeVariable} +
                   "}}\nLocation: " + std::string{location} +
                   "\nCause: No usable value was available in the current run when this request "
                   "was prepared.";
        };
        const auto addDiagnostic = [&](const VariableResolver::UnresolvedOccurrence& occurrence,
                                       const std::string_view resolvedOutput,
                                       const VariableUseKind useKind,
                                       const std::string_view useName,
                                       const std::string_view location) {
            if (ignoreCurrentActorUnresolved) {
                const auto parts = parseVariableParts(occurrence.token);
                if (parts && parts->rawScope == op.actor.value) {
                    return;
                }
            }
            foundUnresolved = true;
            const bool exposeToken = canExposeDiagnosticToken(resolvedOutput, occurrence.token);
            if (!firstUnresolved) {
                firstUnresolved = std::pair{
                    exposeToken ? safeDiagnosticText(occurrence.token) : std::string{"<redacted>"},
                    std::string{location}};
            }
            // ponytail: Bound untrusted template amplification. If richer bulk
            // diagnostics are needed, index sources once before raising this cap.
            if (diagnostics.size() >= kMaxDiagnosticsPerStep) {
                return;
            }
            diagnostics.push_back(makeUnresolvedDiagnostic(occurrence,
                                                           exposeToken,
                                                           useKind,
                                                           useName,
                                                           envName,
                                                           project,
                                                           ctx,
                                                           extractionTraceStart,
                                                           chain,
                                                           stepIndex));
        };
        const auto addValueDiagnostics = [&](const VariableResolver::Result& resolved,
                                             const VariableUseKind useKind,
                                             const std::string_view useName,
                                             const std::string_view location) {
            for (const auto& occurrence : resolved.unresolvedOccurrences) {
                addDiagnostic(occurrence, resolved.output, useKind, useName, location);
            }
        };

        HttpRequest req{};

        // Resolve the operation's effective inline auth once (Inherit → project
        // default) and use it for every auth step below.
        const std::optional<InlineAuth> effAuth = effectiveInlineAuth(op, project);
        const auto failPreparation = [&](const ErrorCode code, std::string detail) {
            result.status = StepResult::Status::Failed;
            result.error = code;
            result.detail = std::move(detail);
            const auto elapsed = std::chrono::steady_clock::now() - startTime;
            result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
            return false;
        };
        const auto prepareRequest = [&](const bool preflightOnly) {
            // A 401 refresh must rebuild from templates rather than overlaying the
            // sent request: session values can affect every request component.
            req = HttpRequest{};
            req.method = op.method;
            req.transport = rctx.transport;

            auto resolvedPath = varResolver.resolveUrlPath(op.pathTemplate, ctx, rctx);
            for (const auto& occurrence : resolvedPath.unresolvedOccurrences) {
                const auto useKind = useKindForComponent(occurrence.component);
                addDiagnostic(
                    occurrence, resolvedPath.output, useKind, {}, locationForUseKind(useKind));
            }

            if (effAuth) {
                for (const auto& use : inlineAuthTemplateUses(*effAuth)) {
                    const auto resolved = varResolver.resolve(use.value, ctx, rctx);
                    addValueDiagnostics(resolved, use.useKind, use.useName, use.location);
                }
            }
            applyInlineMtls(req, effAuth, ctx, rctx);

            const auto baseUrlIt = rctx.envVars.find("baseUrl");
            const std::string baseUrl = baseUrlIt != rctx.envVars.end() ? baseUrlIt->second : "";
            req.url = baseUrl + resolvedPath.output;

            std::map<std::string, VariableResolver::Result> headerResolutions{};
            for (const auto& [key, value] : op.headers) {
                auto resolved = varResolver.resolve(value, ctx, rctx);
                req.headers[key] = resolved.output;
                headerResolutions[key] = std::move(resolved);
            }

            if (!op.actor.value.empty()) {
                const auto actorIt = project.actors.find(op.actor);
                if (actorIt != project.actors.end()) {
                    for (const auto& [key, value] : actorIt->second.inject.headers) {
                        auto resolved = varResolver.resolve(value, ctx, rctx);
                        eraseHeaderCaseInsensitive(req.headers, key);
                        eraseHeaderCaseInsensitive(headerResolutions, key);
                        req.headers[key] = resolved.output;
                        headerResolutions[key] = std::move(resolved);
                    }
                    // Session-level inject wins on key collision.
                    if (const auto* session = ctx.session(op.actor); session != nullptr) {
                        for (const auto& [key, value] : session->injectHeaders) {
                            eraseHeaderCaseInsensitive(req.headers, key);
                            eraseHeaderCaseInsensitive(headerResolutions, key);
                            req.headers[key] = value;
                        }
                        applyActorSessionTransport(req, *session);
                    }
                }

                if (!containsHeaderCaseInsensitive(req.headers, "Cookie")) {
                    const auto jar = ctx.cookies(op.actor);
                    if (!jar.empty()) {
                        req.headers["Cookie"] = cookies::formatRequestHeader(jar);
                    }
                }
            }

            if (effAuth) {
                switch (effAuth->type) {
                    case InlineAuthType::Bearer:
                    case InlineAuthType::Basic:
                    case InlineAuthType::AwsSigV4:
                    case InlineAuthType::OAuth1:
                    case InlineAuthType::OAuth2:
                    case InlineAuthType::Jwt:
                        eraseHeaderCaseInsensitive(headerResolutions, "Authorization");
                        break;
                    case InlineAuthType::ApiKey:
                        if (!effAuth->apiKeyInQuery && !effAuth->apiKeyName.empty()) {
                            const auto key = varResolver.resolve(effAuth->apiKeyName, ctx, rctx);
                            if (key.unresolvedOccurrences.empty()) {
                                eraseHeaderCaseInsensitive(headerResolutions, key.output);
                            }
                        }
                        break;
                    case InlineAuthType::Mtls:
                    case InlineAuthType::None:
                    case InlineAuthType::Inherit:
                        break;
                }
            }
            for (const auto& [name, resolved] : headerResolutions) {
                addValueDiagnostics(resolved,
                                    VariableUseKind::Header,
                                    name,
                                    std::string{"Header \""} + safeDiagnosticText(name) + '"');
            }

            std::map<std::string, std::string> queryParams{};
            std::map<std::string, VariableResolver::Result> queryResolutions{};
            for (const auto& [key, value] : op.queryParams) {
                auto resolved = varResolver.resolve(value, ctx, rctx);
                queryParams[key] = resolved.output;
                queryResolutions[key] = std::move(resolved);
            }
            if (!op.actor.value.empty()) {
                if (const auto* session = ctx.session(op.actor); session != nullptr) {
                    for (const auto& [key, value] : session->injectQueryParams) {
                        queryParams[key] = value;
                        queryResolutions.erase(key);
                    }
                }
            }
            for (const auto& [name, resolved] : queryResolutions) {
                const std::string location =
                    std::string{"Query parameter \""} + safeDiagnosticText(name) + '"';
                addValueDiagnostics(resolved, VariableUseKind::NamedQuery, name, location);
            }
            if (!queryParams.empty()) {
                std::string query{};
                for (const auto& [key, value] : queryParams) {
                    if (!query.empty()) {
                        query += '&';
                    }
                    query += urlEncode(key) + "=" + urlEncode(value);
                }
                appendQueryString(req.url, query);
            }

            std::map<std::string, std::string> resolvedFormFields{};
            if (op.bodyTemplate) {
                auto resolved = varResolver.resolve(*op.bodyTemplate, ctx, rctx);
                addValueDiagnostics(resolved, VariableUseKind::Body, {}, "Request body");
                req.body = resolved.output;
            } else if (op.bodyForm) {
                for (const auto& [key, value] : *op.bodyForm) {
                    auto resolved = varResolver.resolve(value, ctx, rctx);
                    addValueDiagnostics(
                        resolved,
                        VariableUseKind::FormField,
                        key,
                        std::string{"Form field \""} + safeDiagnosticText(key) + '"');
                    resolvedFormFields[key] = resolved.output;
                }
            }

            if (foundUnresolved) {
                std::string detail{};
                if (firstUnresolved) {
                    detail =
                        unresolvedVariableDetail(firstUnresolved->first, firstUnresolved->second);
                }
                return failPreparation(ErrorCode::VarUnresolved, std::move(detail));
            }
            if (preflightOnly) {
                return true;
            }

            // Preserve the prior pure-auth error precedence while keeping
            // side-effectful OAuth acquisition after request-template preflight.
            if (auto authErr = applyInlineAuth(req, effAuth, ctx, rctx)) {
                return failPreparation(authErr->code, authErr->detail);
            }

            if (op.bodyTemplate) {
                if (!containsHeaderCaseInsensitive(req.headers, "Content-Type")) {
                    req.headers["Content-Type"] = "application/json";
                }
            } else if (op.bodyForm) {
                const bool routeMultipart = wantsMultipart(op.headers, resolvedFormFields);
                auto formBody = buildFormBody(resolvedFormFields, routeMultipart);
                if (!formBody) {
                    return failPreparation(formBody.error().code, formBody.error().detail);
                }
                if (auto* multipart = std::get_if<MultipartBody>(&*formBody)) {
                    req.multipart = std::move(multipart->parts);
                    // libcurl writes Content-Type with the boundary; drop any
                    // user-supplied value so curl doesn't send two headers.
                    eraseHeaderCaseInsensitive(req.headers, "Content-Type");
                } else if (auto* encoded = std::get_if<UrlEncodedBody>(&*formBody)) {
                    req.body = std::move(encoded->body);
                    eraseHeaderCaseInsensitive(req.headers, "Content-Type");
                    req.headers["Content-Type"] = "application/x-www-form-urlencoded";
                }
            }

            if (auto oauth2Err = applyInlineOAuth2(req, effAuth, ctx, rctx, runId, stepIndex)) {
                return failPreparation(oauth2Err->code, oauth2Err->detail);
            }

            if (op.timeout) {
                req.timeout = *op.timeout;
            }

            // Hooks rerun after session refresh because their output may depend
            // on freshly resolved headers, URL, or body values.
            if (op.preRequestScript && deps.hooks) {
                auto hookContext = buildHookContext(req, ctx, rctx, project);
                auto outcome =
                    deps.hooks->runPreRequest(*op.preRequestScript, std::move(hookContext));
                if (!outcome) {
                    return failPreparation(outcome.error().code, outcome.error().detail);
                }
                req.url = std::move(outcome->mutatedRequest.url);
                req.headers = std::move(outcome->mutatedRequest.headers);
                req.body = std::move(outcome->mutatedRequest.body);
            }
            return true;
        };

        // Diagnose target templates before authentication can issue HTTP. The
        // current actor's own session fields are the only unresolved references
        // authentication is allowed to satisfy; everything else fails here.
        ignoreCurrentActorUnresolved = true;
        if (!prepareRequest(true)) {
            return result;
        }
        ignoreCurrentActorUnresolved = false;
        foundUnresolved = false;
        firstUnresolved.reset();

        if (!op.actor.value.empty()) {
            const auto actorIt = project.actors.find(op.actor);
            if (actorIt != project.actors.end() &&
                !ensureSession(actorIt->second, ctx, rctx, runId, stepIndex)) {
                result.status = StepResult::Status::Failed;
                result.error = ErrorCode::SessionRefreshFailed;
                return result;
            }
        }

        if (!prepareRequest(false)) {
            return result;
        }

        const int maxAttempts = op.retry.maxAttempts;
        std::optional<HttpResponse> httpResp;
        ReqloomError lastError{};
        int attemptCount = 0;

        for (int attempt = 0; attempt <= maxAttempts; ++attempt) {
            ++attemptCount;
            if (isCancelled(runId)) {
                result.status = StepResult::Status::Cancelled;
                result.error = ErrorCode::Cancelled;
                result.attempts = attemptCount;
                return result;
            }

            // Per-request signing (OAuth 1.0a / AWS SigV4). Inside the
            // retry loop so each attempt gets a fresh nonce/timestamp.
            if (!op.actor.value.empty()) {
                if (const auto* session = ctx.session(op.actor); session) {
                    if (session->signingScheme == ActorSession::SigningScheme::OAuth1HmacSha1) {
                        if (!signOAuth1Request(req, *session)) {
                            result.status = StepResult::Status::Failed;
                            result.error = ErrorCode::SessionRefreshFailed;
                            result.detail =
                                "oauth1 signing failed (missing "
                                "consumer credentials or "
                                "malformed URL)";
                            result.attempts = attemptCount;
                            return result;
                        }
                    } else if (session->signingScheme == ActorSession::SigningScheme::AwsSigV4) {
                        if (!signSigV4Request(req, *session)) {
                            result.status = StepResult::Status::Failed;
                            result.error = ErrorCode::SessionRefreshFailed;
                            result.detail =
                                "aws_sigv4 signing failed "
                                "(missing access_key/secret_key/"
                                "region/service or malformed URL)";
                            result.attempts = attemptCount;
                            return result;
                        }
                    }
                }
            }

            // Inline (actor-less) AWS SigV4 / OAuth1 signing — per-attempt so
            // each retry gets a fresh nonce/timestamp, mirroring the actor path.
            if (!signInlineAuth(req, effAuth, ctx, rctx)) {
                result.status = StepResult::Status::Failed;
                result.error = ErrorCode::SessionRefreshFailed;
                result.detail = "inline auth signing failed (missing credentials or malformed URL)";
                result.attempts = attemptCount;
                return result;
            }

            emit(RequestPrepared{runId,
                                 stepIndex,
                                 req.method,
                                 req.url,
                                 headersToVector(maskHeaders(req.headers)),
                                 requestBodySize(req),
                                 std::chrono::system_clock::now()});

            auto resp = deps.http->send(req);
            if (resp) {
                httpResp = std::move(*resp);
                emit(ResponseReceived{runId,
                                      stepIndex,
                                      httpResp->status,
                                      maskHeaders(httpResp->headers),
                                      httpResp->body.size(),
                                      httpResp->elapsed,
                                      std::chrono::system_clock::now(),
                                      capturedBody(httpResp->body)});
                // Absorb Set-Cookie headers immediately so any
                // post_response hook running between here and the next
                // outbound call sees the up-to-date jar (today the jar
                // isn't exposed to the JS sandbox, but the contract is
                // clearer if absorption happens at receive time, not
                // before send time).
                if (!op.actor.value.empty()) {
                    absorbResponseCookies(httpResp->headers, op.actor, ctx);
                }
                break;
            }
            lastError = resp.error();
            if (!isRetryable(lastError.code) || attempt >= maxAttempts) {
                result.status = StepResult::Status::Failed;
                result.error = lastError.code;
                result.attempts = attemptCount;
                return result;
            }
            // Exponential backoff. Cap the shift at 20 to avoid signed-overflow
            // UB on large maxAttempts values.
            const auto shift = std::min(attempt, 20);
            auto delay = op.retry.baseBackoff * (std::uint32_t{1} << shift);
            if (delay > op.retry.maxBackoff) {
                delay = op.retry.maxBackoff;
            }
            std::this_thread::sleep_for(delay);
        }

        result.attempts = attemptCount;

        // 401-recovery: if the response says "your session is no longer
        // valid", try re-authenticating once and retry the operation.
        // Only fires when:
        //   - the op has an actor (otherwise there's nothing to refresh)
        //   - the response is a real HTTP 401 (not a redirected 200)
        //   - the user's `expect_status:` doesn't already include 401
        //     (some flows test 401 explicitly — don't fight them)
        // Emits SessionRefreshed{Trigger::Unauthorized} so subscribers
        // can surface this in the timeline UI.
        const auto userExpects401 = [&]() {
            if (!op.expectStatusList.empty()) {
                return std::find(op.expectStatusList.begin(), op.expectStatusList.end(), 401) !=
                       op.expectStatusList.end();
            }
            return op.expectStatus.has_value() && *op.expectStatus == 401;
        }();

        if (httpResp && httpResp->status == 401 && !op.actor.value.empty() && !userExpects401) {
            auto actorIt = project.actors.find(op.actor);
            if (actorIt != project.actors.end()) {
                ctx.invalidateSession(op.actor);
                if (ensureSession(actorIt->second, ctx, rctx, runId, stepIndex)) {
                    emit(SessionRefreshed{runId,
                                          op.actor,
                                          SessionRefreshed::Trigger::Unauthorized,
                                          std::chrono::system_clock::now()});

                    if (!prepareRequest(false)) {
                        return result;
                    }
                    if (const auto* session = ctx.session(op.actor); session != nullptr) {
                        if (session->signingScheme == ActorSession::SigningScheme::OAuth1HmacSha1) {
                            eraseHeaderCaseInsensitive(req.headers, "Authorization");
                            if (!signOAuth1Request(req, *session)) {
                                result.status = StepResult::Status::Failed;
                                result.error = ErrorCode::SessionRefreshFailed;
                                result.detail = "oauth1 signing failed after session refresh";
                                return result;
                            }
                        } else if (session->signingScheme ==
                                   ActorSession::SigningScheme::AwsSigV4) {
                            eraseHeaderCaseInsensitive(req.headers, "Authorization");
                            if (!signSigV4Request(req, *session)) {
                                result.status = StepResult::Status::Failed;
                                result.error = ErrorCode::SessionRefreshFailed;
                                result.detail = "aws_sigv4 signing failed after session refresh";
                                return result;
                            }
                        }
                    }
                    if (!signInlineAuth(req, effAuth, ctx, rctx)) {
                        result.status = StepResult::Status::Failed;
                        result.error = ErrorCode::SessionRefreshFailed;
                        result.detail = "inline auth signing failed after session refresh";
                        return result;
                    }

                    emit(RequestPrepared{runId,
                                         stepIndex,
                                         req.method,
                                         req.url,
                                         headersToVector(maskHeaders(req.headers)),
                                         requestBodySize(req),
                                         std::chrono::system_clock::now()});

                    auto retryResp = deps.http->send(req);
                    ++attemptCount;
                    result.attempts = attemptCount;
                    if (retryResp) {
                        httpResp = std::move(*retryResp);
                        emit(ResponseReceived{runId,
                                              stepIndex,
                                              httpResp->status,
                                              maskHeaders(httpResp->headers),
                                              httpResp->body.size(),
                                              httpResp->elapsed,
                                              std::chrono::system_clock::now(),
                                              capturedBody(httpResp->body)});
                        absorbResponseCookies(httpResp->headers, op.actor, ctx);
                    } else {
                        // Network error on the retry — surface the new
                        // error rather than the original 401 so users
                        // see what changed.
                        result.status = StepResult::Status::Failed;
                        result.error = retryResp.error().code;
                        result.detail = retryResp.error().detail;
                        auto elapsed = std::chrono::steady_clock::now() - startTime;
                        result.elapsed =
                            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
                        return result;
                    }
                }
            }
        }

        // When expectStatusList is non-empty it takes precedence over the
        // singular expectStatus field; the latter is the legacy single-value
        // form consulted only when the list is empty.
        const auto statusMatches = [&]() -> bool {
            if (!httpResp) {
                return true;
            }
            if (!op.expectStatusList.empty()) {
                return std::find(op.expectStatusList.begin(),
                                 op.expectStatusList.end(),
                                 httpResp->status) != op.expectStatusList.end();
            }
            if (op.expectStatus) {
                return httpResp->status == *op.expectStatus;
            }
            return true;
        }();

        if (!statusMatches) {
            result.status = StepResult::Status::Failed;
            result.error = (httpResp->status >= 500) ? ErrorCode::Http5xx : ErrorCode::Http4xx;
            constexpr std::size_t kBodyExcerpt = 200;
            std::string const bodyExcerpt = httpResp->body.size() > kBodyExcerpt
                                                ? httpResp->body.substr(0, kBodyExcerpt) + "..."
                                                : httpResp->body;
            result.detail = "HTTP " + std::to_string(httpResp->status) + " — " + bodyExcerpt;
            auto elapsed = std::chrono::steady_clock::now() - startTime;
            result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
            return result;
        }

        // Polling phase — engine polls until success_when/fail_when matches or budget fires.
        if (op.pollUntil && httpResp) {
            auto pollResult = runPollLoop(op,
                                          *op.pollUntil,
                                          project,
                                          ctx,
                                          rctx,
                                          runId,
                                          stepIndex,
                                          *httpResp,
                                          pollAttemptRows);
            if (!pollResult.has_value()) {
                result.status = StepResult::Status::Failed;
                result.error = pollResult.error().code;
                result.detail = pollResult.error().detail;
                auto elapsed = std::chrono::steady_clock::now() - startTime;
                result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
                return result;
            }
            httpResp = std::move(*pollResult);
        }

        // post_response hook: runs after the final response (post-poll if
        // applicable) but before extraction. Hooks may mutate
        // status/headers/body — extractions then see the mutated body.
        // Useful for decrypting / unwrapping vendor envelopes.
        if (httpResp && op.postResponseScript && deps.hooks) {
            auto hctx = buildHookContext(req, ctx, rctx, project);
            HookResponseView respView;
            respView.status = httpResp->status;
            respView.headers = headersToMap(httpResp->headers);
            respView.body = httpResp->body;
            hctx.response = std::move(respView);

            auto outcome = deps.hooks->runPostResponse(*op.postResponseScript, std::move(hctx));
            if (!outcome) {
                result.status = StepResult::Status::Failed;
                result.error = outcome.error().code;
                result.detail = outcome.error().detail;
                auto elapsed = std::chrono::steady_clock::now() - startTime;
                result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
                return result;
            }
            if (outcome->mutatedResponse) {
                httpResp->status = outcome->mutatedResponse->status;
                httpResp->headers = headersToVector(outcome->mutatedResponse->headers);
                httpResp->body = std::move(outcome->mutatedResponse->body);
            }
        }

        if (httpResp && !op.extractions.empty()) {
            // Partition extractions: list (`[*]` JSONPath, fan-out) vs scalar.
            std::vector<Extraction> scalarExts;
            std::vector<Extraction> listExts;
            for (const auto& ext : op.extractions) {
                if (ext.source == Extraction::Source::JsonPath &&
                    ext.sourcePath.find("[*]") != std::string::npos) {
                    listExts.push_back(ext);
                } else {
                    scalarExts.push_back(ext);
                }
            }

            std::map<std::string, std::string> scalarValues;
            if (!scalarExts.empty()) {
                auto detailed = extractFromResponseDetailed(
                    op.id, httpResp->body, httpResp->status, httpResp->headers, scalarExts);
                if (!detailed) {
                    result.status = StepResult::Status::Failed;
                    result.error = detailed.error().code;
                    auto elapsed = std::chrono::steady_clock::now() - startTime;
                    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
                    return result;
                }

                // Trace every extraction outcome — including misses — so the
                // timeline shows nulls and missing fields. The op still fails
                std::optional<std::string> firstMiss;
                for (auto& t : detailed->traces) {
                    const bool isMissLike = t.outcome == ExtractionTrace::Outcome::Missing ||
                                            t.outcome == ExtractionTrace::Outcome::InvalidPattern;
                    if (!firstMiss && isMissLike) {
                        firstMiss = t.variableName;
                    }

                    ExtractionCompleted ev;
                    ev.runId = runId;
                    ev.stepIndex = stepIndex;
                    ev.op = t.op;
                    ev.variableName = t.variableName;
                    ev.sourcePath = t.sourcePath;
                    ev.at = std::chrono::system_clock::now();
                    switch (t.outcome) {
                        case ExtractionTrace::Outcome::Resolved:
                            // RunContext keeps the real value for downstream
                            // templating; the event copy (timeline + disk) is
                            // masked when the variable name looks secret.
                            ev.outcome = ExtractionCompleted::Outcome::Resolved;
                            ev.value = isSensitiveName(t.variableName)
                                           ? std::string{kRedactedHeaderValue}
                                           : t.value;
                            break;
                        case ExtractionTrace::Outcome::Null:
                            ev.outcome = ExtractionCompleted::Outcome::Null;
                            break;
                        case ExtractionTrace::Outcome::Missing:
                            ev.outcome = ExtractionCompleted::Outcome::Missing;
                            break;
                        case ExtractionTrace::Outcome::InvalidPattern:
                            ev.outcome = ExtractionCompleted::Outcome::InvalidPattern;
                            break;
                        case ExtractionTrace::Outcome::Unsupported:
                            ev.outcome = ExtractionCompleted::Outcome::Unsupported;
                            break;
                    }
                    emit(std::move(ev));

                    ctx.recordExtraction(std::move(t));
                }
                if (firstMiss) {
                    result.status = StepResult::Status::Failed;
                    result.error = ErrorCode::ExtractionFailed;
                    result.detail = "extract '" + *firstMiss + "' missed for " + op.id.value;
                    auto elapsed = std::chrono::steady_clock::now() - startTime;
                    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
                    return result;
                }
                scalarValues = std::move(detailed->values);
            }

            if (listExts.empty()) {
                // Plain extraction: one instance from the scalar values.
                if (!scalarValues.empty()) {
                    ResourceInstance instance;
                    instance.variables = std::move(scalarValues);

                    std::vector<std::string> names;
                    names.reserve(instance.variables.size());
                    for (const auto& [k, _] : instance.variables) {
                        names.push_back(k);
                    }
                    emit(ExtractionApplied{runId,
                                           stepIndex,
                                           op.resource,
                                           std::move(names),
                                           std::chrono::system_clock::now()});
                    ctx.appendInstance(op.resource, std::move(instance));
                }
            } else {
                // List extraction (for-each producer): each `[*]` path yields a
                // vector; we append one instance per item (scalars broadcast
                // into every instance), so a downstream for_each can iterate.
                std::size_t count = 0;
                std::vector<std::pair<std::string, std::vector<std::string>>> lists;
                std::optional<std::string> emptyList;
                for (const auto& ext : listExts) {
                    auto values = extractValues(httpResp->body, ext.sourcePath);
                    count = std::max(count, values.size());
                    if (values.empty() && !emptyList) {
                        emptyList = ext.variableName;
                    }
                    ExtractionCompleted ev;
                    ev.runId = runId;
                    ev.stepIndex = stepIndex;
                    ev.op = op.id;
                    ev.variableName = ext.variableName;
                    ev.sourcePath = ext.sourcePath;
                    ev.at = std::chrono::system_clock::now();
                    ev.outcome = values.empty() ? ExtractionCompleted::Outcome::Missing
                                                : ExtractionCompleted::Outcome::Resolved;
                    ev.value =
                        values.empty() ? std::string{} : std::to_string(values.size()) + " items";
                    emit(std::move(ev));
                    lists.emplace_back(ext.variableName, std::move(values));
                }
                if (emptyList) {
                    result.status = StepResult::Status::Failed;
                    result.error = ErrorCode::ExtractionFailed;
                    result.detail =
                        "list extract '" + *emptyList + "' matched no items for " + op.id.value;
                    auto elapsed = std::chrono::steady_clock::now() - startTime;
                    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
                    return result;
                }

                std::vector<std::string> names;
                names.reserve(scalarValues.size() + lists.size());
                for (const auto& [k, _] : scalarValues) {
                    names.push_back(k);
                }
                for (const auto& [var, _] : lists) {
                    names.push_back(var);
                }
                emit(ExtractionApplied{runId,
                                       stepIndex,
                                       op.resource,
                                       std::move(names),
                                       std::chrono::system_clock::now()});

                for (std::size_t k = 0; k < count; ++k) {
                    ResourceInstance instance;
                    instance.variables = scalarValues;  // broadcast scalars
                    for (const auto& [var, values] : lists) {
                        if (k < values.size()) {
                            instance.variables[var] = values[k];
                        }
                    }
                    ctx.appendInstance(op.resource, std::move(instance));
                }
            }
        }

        // Response assertions: evaluate each declared predicate against the
        // final (post-poll, post-hook, post-extraction) response. Record every
        // outcome; the first failure fails the step. Reuses the predicate
        // grammar that poll_until.success_when uses.
        if (httpResp && !op.assertions.empty()) {
            const PredicateEvaluator assertionEval;
            const auto parsedBody = assertionEval.parseBody(httpResp->body);
            std::optional<std::string> firstFail;
            for (const auto& assertion : op.assertions) {
                AssertionResult ar;
                ar.expr = assertion.expr;
                ar.name = assertion.name.value_or(assertion.expr);
                const auto parsed = assertionEval.parse(assertion.expr);
                ar.passed =
                    parsed && assertionEval.evaluate(*parsed, parsedBody, httpResp->status) ==
                                  PredicateValue::True;
                if (!ar.passed && !firstFail) {
                    firstFail = ar.name;
                }
                emit(AssertionCompleted{runId,
                                        stepIndex,
                                        op.id,
                                        ar.name,
                                        ar.expr,
                                        ar.passed,
                                        std::chrono::system_clock::now()});
                result.assertions.push_back(std::move(ar));
            }
            if (firstFail) {
                result.status = StepResult::Status::Failed;
                result.error = ErrorCode::AssertionFailed;
                result.detail = "assertion failed: " + *firstFail + " for " + op.id.value;
                auto elapsed = std::chrono::steady_clock::now() - startTime;
                result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
                return result;
            }
        }

        result.status = StepResult::Status::Succeeded;
        auto elapsed = std::chrono::steady_clock::now() - startTime;
        result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
        return result;
    }
};

// ─── Public API ──────────────────────────────────────────────────────────────

ExecutionEngine::ExecutionEngine(Dependencies deps)
    : impl_(std::make_unique<Impl>(std::move(deps))) {}

ExecutionEngine::~ExecutionEngine() = default;
ExecutionEngine::ExecutionEngine(ExecutionEngine&&) noexcept = default;
ExecutionEngine& ExecutionEngine::operator=(ExecutionEngine&&) noexcept = default;

std::expected<RunResult, ReqloomError> ExecutionEngine::run(const Project& project,
                                                            const OperationId& target,
                                                            RunContext& ctx,
                                                            const RunOptions& options) {
    impl_->cancelledRunId.store(0, std::memory_order_release);
    impl_->captureResponseBodies = options.captureResponseBodies;
    const auto runStart = std::chrono::steady_clock::now();

    if (options.resetExtractions) {
        ctx.clearExtractions();
    }
    const std::size_t extractionTraceStart{ctx.extractionTrace().size()};
    if (options.resetSessions) {
        for (const auto& [actorId, _] : project.actors) {
            ctx.invalidateSession(actorId);
        }
    }

    auto chainResult = impl_->resolver.resolve(project, target);
    if (!chainResult) {
        return std::unexpected(chainResult.error());
    }

    const auto& chain = *chainResult;
    auto runId = RunId{impl_->nextRunId.fetch_add(1)};

    ResolveContext rctx;
    auto envName = options.environment.empty() ? project.defaultEnvironment : options.environment;
    if (project.environments.contains(envName)) {
        rctx.envVars = project.environments.at(envName);
    }
    // Resolve per-env transport overrides once at run start. Operations,
    // auth steps, refresh blocks and poll requests all see the same
    // TLS / proxy / connect-timeout settings via rctx.transport.
    if (auto it = project.transport.find(envName); it != project.transport.end()) {
        rctx.transport = it->second;
    }

    // Pre-load referenced secrets from the OS keychain into the resolve
    // context. We read only the names the project actually references —
    // never a bulk dump — so an unrelated keychain entry can't leak into
    // a run. A missing key is left unset (surfaces later as VarUnresolved
    // when the template can't resolve); a backend failure aborts the run
    // with SecretAccessFailed so the user isn't silently sent unsigned.
    //
    // Skipped on dry runs: a preview must not touch the OS keychain, which
    // can pop an interactive unlock/authorization prompt. Unresolved
    // `{{secret.X}}` markers in the previewed chain are expected.
    if (impl_->deps.secrets && !options.dryRun) {
        for (const auto& name : DependencyResolver::collectSecretReferences(project)) {
            auto value = impl_->deps.secrets->read(name);
            if (!value) {
                return std::unexpected(ReqloomError{
                    ErrorCode::SecretAccessFailed,
                    ErrorClass::Auth,
                    "secret store: failed to read '" + name + "': " + value.error().detail});
            }
            if (value->has_value()) {
                rctx.secrets[name] = std::move(**value);
            }
        }
    }

    impl_->emit(RunStarted{runId, target, chain.size(), envName, std::chrono::system_clock::now()});

    RunResult result;
    result.runId = runId;
    result.outcome = RunOutcome::Succeeded;
    // One row per chained op at minimum; poll attempts append a few more.
    result.steps.reserve(chain.size());

    for (std::size_t i = 0; i < chain.size(); ++i) {
        const auto& opId = chain[i];

        auto dotPos = opId.value.find('.');
        auto resName = opId.value.substr(0, dotPos);
        auto opName = opId.value.substr(dotPos + 1);

        auto resIt = project.resources.find(ResourceId{resName});
        if (resIt == project.resources.end()) {
            result.outcome = RunOutcome::Failed;
            break;
        }
        auto opIt = resIt->second.operations.find(opName);
        if (opIt == resIt->second.operations.end()) {
            result.outcome = RunOutcome::Failed;
            break;
        }

        const auto& op = opIt->second;
        const bool isTarget = (opId.value == target.value);

        if (!isTarget && !op.force && !op.extractions.empty()) {
            const auto& instances = ctx.instances(op.resource);
            if (!instances.empty()) {
                bool allPresent = true;
                for (const auto& ext : op.extractions) {
                    bool found = false;
                    for (const auto& inst : instances) {
                        if (inst.variables.contains(ext.variableName)) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        allPresent = false;
                        break;
                    }
                }
                if (allPresent) {
                    StepResult skipResult;
                    skipResult.op = opId;
                    skipResult.status = StepResult::Status::Skipped;
                    result.steps.push_back(std::move(skipResult));
                    impl_->emit(StepSkipped{runId,
                                            i,
                                            opId,
                                            SkipReason::ExtractionCached,
                                            std::chrono::system_clock::now()});
                    continue;
                }
            }
        }

        if (options.dryRun) {
            StepResult dryResult;
            dryResult.op = opId;
            dryResult.status = StepResult::Status::Succeeded;
            result.steps.push_back(std::move(dryResult));
            continue;
        }

        impl_->emit(StepStarted{runId, i, opId, 1, std::chrono::system_clock::now()});

        StepResult stepResult{};
        std::vector<UnresolvedVariableDiagnostic> stepDiagnostics{};
        if (op.forEach) {
            // Data-driven fan-out: run once per instance of the `over` resource
            // (populated by an upstream list extraction). Each iteration binds
            // `{{<over>.field}}` to that item; iteration rows are tagged with
            // forEachIndex and grouped under this step, like poll attempts.
            const ResourceId overRes = op.forEach->over;
            const std::size_t itemCount = ctx.instances(overRes).size();
            stepResult.op = opId;
            if (itemCount == 0) {
                stepResult.status = StepResult::Status::Succeeded;
                stepResult.detail = "for_each: no items in " + overRes.value;
            } else {
                bool anyFailed = false;
                bool anyCancelled = false;
                const bool continueOnError = op.forEach->continueOnError;
                std::size_t failedCount = 0;
                for (std::size_t k = 0; k < itemCount; ++k) {
                    ctx.setIteration(overRes, k);
                    std::vector<StepResult> iterPollRows{};
                    std::vector<UnresolvedVariableDiagnostic> iterDiagnostics{};
                    auto iter = impl_->executeStep(op,
                                                   project,
                                                   ctx,
                                                   rctx,
                                                   runId,
                                                   i,
                                                   chain,
                                                   extractionTraceStart,
                                                   envName,
                                                   iterPollRows,
                                                   iterDiagnostics);
                    for (auto& row : iterPollRows) {
                        row.forEachIndex = static_cast<int>(k + 1);
                        result.steps.push_back(std::move(row));
                    }
                    iter.forEachIndex = static_cast<int>(k + 1);
                    if (iter.status == StepResult::Status::Failed) {
                        const bool preferThisFailure =
                            !anyFailed || (iter.error == ErrorCode::VarUnresolved &&
                                           stepResult.error != ErrorCode::VarUnresolved);
                        anyFailed = true;
                        ++failedCount;
                        if (preferThisFailure) {
                            stepResult.error = iter.error;
                            stepResult.detail = iter.detail;
                            stepDiagnostics = std::move(iterDiagnostics);
                        }
                    }
                    if (iter.status == StepResult::Status::Cancelled) {
                        anyCancelled = true;
                    }
                    ctx.record(iter);
                    result.steps.push_back(std::move(iter));
                    // A cancel always stops the fan-out. A failure stops it only
                    // when continue-on-error is off.
                    if (anyCancelled || (anyFailed && !continueOnError)) {
                        break;
                    }
                }
                ctx.setIteration(overRes, std::nullopt);
                stepResult.status = anyCancelled ? StepResult::Status::Cancelled
                                    : anyFailed  ? StepResult::Status::Failed
                                                 : StepResult::Status::Succeeded;
                if (stepResult.status == StepResult::Status::Cancelled) {
                    stepResult.error.reset();
                    stepResult.detail.clear();
                    stepDiagnostics.clear();
                } else if (stepResult.status == StepResult::Status::Succeeded) {
                    stepResult.detail = "for_each over " + overRes.value + " — " +
                                        std::to_string(itemCount) + " iterations";
                } else if (anyFailed && continueOnError &&
                           stepResult.error != ErrorCode::VarUnresolved) {
                    stepResult.detail = "for_each over " + overRes.value + " — " +
                                        std::to_string(failedCount) + " of " +
                                        std::to_string(itemCount) + " iterations failed";
                }
            }
            result.steps.push_back(stepResult);
            ctx.record(stepResult);
        } else {
            std::vector<StepResult> pollAttemptRows{};
            stepResult = impl_->executeStep(op,
                                            project,
                                            ctx,
                                            rctx,
                                            runId,
                                            i,
                                            chain,
                                            extractionTraceStart,
                                            envName,
                                            pollAttemptRows,
                                            stepDiagnostics);
            // Per-attempt rows precede the parent step row so renderers can
            // group them under the operation that owned the poll loop.
            for (auto& row : pollAttemptRows) {
                result.steps.push_back(std::move(row));
            }
            result.steps.push_back(stepResult);
            ctx.record(stepResult);
        }

        if (stepResult.status == StepResult::Status::Failed) {
            impl_->emit(StepFailed{runId,
                                   i,
                                   opId,
                                   stepResult.error.value_or(ErrorCode::Http4xx),
                                   classify(stepResult.error.value_or(ErrorCode::Http4xx)),
                                   stepResult.attempts,
                                   stepResult.detail,
                                   std::chrono::system_clock::now(),
                                   std::move(stepDiagnostics)});
            result.outcome = RunOutcome::Failed;
            for (std::size_t j = i + 1; j < chain.size(); ++j) {
                StepResult blocked{};
                blocked.op = chain[j];
                blocked.status = StepResult::Status::Blocked;
                result.steps.push_back(std::move(blocked));
                impl_->emit(StepBlocked{runId, j, chain[j], i, std::chrono::system_clock::now()});
            }
            break;
        }

        if (stepResult.status == StepResult::Status::Cancelled) {
            impl_->emit(StepCancelled{runId, i, opId, std::chrono::system_clock::now()});
            result.outcome = RunOutcome::Cancelled;
            for (std::size_t j = i + 1; j < chain.size(); ++j) {
                StepResult cancelled;
                cancelled.op = chain[j];
                cancelled.status = StepResult::Status::Cancelled;
                result.steps.push_back(std::move(cancelled));
                impl_->emit(StepCancelled{runId, j, chain[j], std::chrono::system_clock::now()});
            }
            break;
        }
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - runStart);
    impl_->emit(RunEnded{runId, result.outcome, elapsed, std::chrono::system_clock::now()});
    return result;
}

std::expected<ResolvedPlan, ReqloomError> ExecutionEngine::resolvePlan(
    const Project& project, const OperationId& target) const {
    return impl_->resolver.resolvePlan(project, target);
}

std::expected<std::vector<VariableSuggestion>, ReqloomError> ExecutionEngine::suggestVariables(
    const Project& project, const OperationId& target, const std::string& environment) const {
    auto plan = impl_->resolver.resolvePlan(project, target);
    if (!plan) {
        return std::unexpected(plan.error());
    }
    return collectVariableSuggestions(project, target, *plan, environment);
}

void ExecutionEngine::cancel(RunId run) {
    impl_->cancelledRunId.store(run.value, std::memory_order_release);
}

void ExecutionEngine::subscribe(EventCallback callback) {
    const std::lock_guard lock(impl_->subscriberMutex);
    impl_->subscribers.push_back(std::move(callback));
}

std::expected<void, ReqloomError> ExecutionEngine::openHistory(
    const std::filesystem::path& dbPath) {
    if (!impl_->deps.history) {
        return std::unexpected(ReqloomError{
            ErrorCode::Internal, ErrorClass::Run, "engine built without a history store"});
    }
    return impl_->deps.history->open(dbPath);
}

std::expected<std::vector<RunHistoryEntry>, ReqloomError> ExecutionEngine::listRuns(
    std::size_t limit) const {
    if (!impl_->deps.history) {
        return std::unexpected(ReqloomError{
            ErrorCode::Internal, ErrorClass::Run, "engine built without a history store"});
    }
    auto rows = impl_->deps.history->listRuns(limit);
    if (!rows) {
        return std::unexpected(rows.error());
    }
    std::vector<RunHistoryEntry> entries;
    entries.reserve(rows->size());
    for (auto& row : *rows) {
        entries.emplace_back(RunHistoryEntry{row.runId,
                                             std::move(row.targetOp),
                                             std::move(row.envName),
                                             std::move(row.startedAt),
                                             std::move(row.endedAt),
                                             std::move(row.outcome),
                                             row.chainSize,
                                             row.elapsedMs});
    }
    return entries;
}

std::expected<std::vector<RunEvent>, ReqloomError> ExecutionEngine::historyEvents(RunId run) const {
    if (!impl_->deps.history) {
        return std::unexpected(ReqloomError{
            ErrorCode::Internal, ErrorClass::Run, "engine built without a history store"});
    }
    return impl_->deps.history->eventsFor(run);
}

std::expected<void, ReqloomError> ExecutionEngine::clearHistory() {
    if (!impl_->deps.history) {
        return std::unexpected(ReqloomError{
            ErrorCode::Internal, ErrorClass::Run, "engine built without a history store"});
    }
    return impl_->deps.history->clear();
}

}  // namespace reqloom::engine
