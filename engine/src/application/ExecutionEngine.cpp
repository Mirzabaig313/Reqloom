// ExecutionEngine — resolves dependency chains, authenticates actors, executes steps.

#include <reqloom/engine/ExecutionEngine.h>

#include "../domain/Codecs.h"
#include "../domain/DependencyResolver.h"
#include "../domain/JsonBodyTemplate.h"
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
#include "HookScope.h"
#include "JsonExtraction.h"

#include <reqloom/engine/JsonValues.h>
#include <reqloom/engine/ProjectIdentity.h>
#include "../domain/SecretRegistry.h"
#include "MultipartBuilder.h"
#include "PredicateEvaluator.h"
#include "PredicateTemplates.h"
#include "RequestSigners.h"
#include "SecretEnvironment.h"
#include "SecretRedaction.h"
#include "StepEvidence.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
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

/// The instance that answers `{{resource.field}}` when it records the field as null or missing.
[[nodiscard]] const ResourceInstance* latestUnsetRecord(const RunContext& ctx,
                                                        const VariableParts& parts) {
    if (parts.rawScope != parts.sourceScope) {
        return nullptr;
    }
    const ResourceId resource{std::string{parts.sourceScope}};
    const std::string field{parts.field};
    const auto& instances = ctx.instances(resource);
    if (const auto bound = ctx.iteration(resource); bound && *bound < instances.size()) {
        return instances[*bound].isUnset(field) ? &instances[*bound] : nullptr;
    }
    for (auto it = instances.rbegin(); it != instances.rend(); ++it) {
        if (it->variables.contains(field)) {
            return nullptr;
        }
        if (it->isUnset(field)) {
            return &*it;
        }
    }
    return nullptr;
}

[[nodiscard]] std::optional<std::string> unsetFieldReason(const RunContext& ctx,
                                                          const std::string_view token) {
    const auto parts = parseVariableParts(token);
    if (!parts) {
        return std::nullopt;
    }
    const auto* record = latestUnsetRecord(ctx, *parts);
    if (record == nullptr) {
        return std::nullopt;
    }
    const std::string field{parts->field};
    const std::string producer =
        record->producer.value.empty() ? std::string{"producing"} : record->producer.value;
    if (record->nullVariables.contains(field)) {
        return "The last " + producer + " response returned null for '" + field + "'.";
    }
    return "The last " + producer + " response did not contain '" + field + "'.";
}

/// Why a `{{secret.X}}`, or an `{{env.K}}` whose value uses a secret, has no value; else nullopt.
[[nodiscard]] std::optional<std::string> unresolvedSecretReason(const std::string_view token,
                                                                const ResolveContext& rctx,
                                                                const SecretSourceState& sources) {
    const auto parts = parseVariableParts(token);
    if (!parts) {
        return std::nullopt;
    }
    if (parts->rawScope == "secret") {
        return missingSecretCause(parts->field, sources);
    }
    if (parts->rawScope != "env") {
        return std::nullopt;
    }
    const auto it = rctx.envVars.find(std::string{parts->field});
    if (it == rctx.envVars.end()) {
        return std::nullopt;
    }
    for (const auto& name : referencedSecretNames({it->second}, {})) {
        if (!rctx.secrets.contains(name)) {
            return "{{" + std::string{token} + "}} uses {{secret." + name + "}}, and " +
                   missingSecretCause(name, sources);
        }
    }
    return std::nullopt;
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
    diagnostic.token =
        exposeToken ? safeDiagnosticText(occurrence.token) : std::string{kRedactedValueMarker};
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
        } else if (const auto* record = latestUnsetRecord(ctx, *parts)) {
            diagnostic.cause = record->nullVariables.contains(std::string{parts->field})
                                   ? UnresolvedVariableCause::ExtractionNull
                                   : UnresolvedVariableCause::ExtractionMissing;
            diagnostic.sourceKind = VariableSourceKind::Extraction;
            if (!record->producer.value.empty()) {
                diagnostic.producerOp = record->producer;
            }
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

/// Overlay an actor session's mTLS transport onto `req`, keeping the environment's proxy/timeout
/// settings; only non-empty fields win.
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

/// Build a HookContext snapshot (values copied so the hook can't reach back into RunContext): the
/// operation's actor session and the secrets `hookSecretNames` allows.
[[nodiscard]] HookContext buildHookContext(const HttpRequest& req,
                                           const RunContext& ctx,
                                           const ResolveContext& rctx,
                                           const Operation& op,
                                           const Project& project,
                                           const std::string_view script) {
    HookContext out;
    out.request.method = req.method;
    out.request.url = req.url;
    out.request.headers = req.headers;
    out.request.body = req.body;

    if (const auto* sess = ctx.session(op.actor); sess != nullptr && !op.actor.value.empty()) {
        out.variables[op.actor.value] = sess->variables;
    }
    out.env = rctx.envVars;
    for (const auto& name : hookSecretNames(op, project, rctx.envVars, script)) {
        if (const auto it = rctx.secrets.find(name); it != rctx.secrets.end()) {
            out.secrets.insert(*it);
        }
    }
    return out;
}

/// Convert HttpResponse's ordered header pairs into the map the hook surface expects.
[[nodiscard]] std::map<std::string, std::string> headersToMap(
    const std::vector<std::pair<std::string, std::string>>& headers) {
    std::map<std::string, std::string> out;
    for (const auto& [k, v] : headers) {
        out[k] = v;
    }
    return out;
}

/// Convert a hook-mutated header map back into vector<pair> form.
/// Add the secrets the chain's hook scripts name, which no template references.
void appendHookScriptSecrets(const Project& project,
                             const std::vector<OperationId>& chain,
                             std::vector<std::string>& names) {
    std::set<std::string> all{names.begin(), names.end()};
    for (const auto& opId : chain) {
        const auto dot = opId.value.find('.');
        const auto res = project.resources.find(ResourceId{opId.value.substr(0, dot)});
        if (dot == std::string::npos || res == project.resources.end()) {
            continue;
        }
        const auto op = res->second.operations.find(opId.value.substr(dot + 1));
        if (op == res->second.operations.end()) {
            continue;
        }
        for (const auto* script : {&op->second.preRequestScript, &op->second.postResponseScript}) {
            if (*script) {
                all.merge(hookScriptSecretNames(**script));
            }
        }
    }
    names.assign(all.begin(), all.end());
}

[[nodiscard]] std::vector<std::pair<std::string, std::string>> headersToVector(
    const std::map<std::string, std::string>& headers) {
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(headers.size());
    for (const auto& [k, v] : headers) {
        out.emplace_back(k, v);
    }
    return out;
}

/// Bytes the request body puts on the wire: inline body or summed multipart part sizes.
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
    const std::string redacted = urlEncode(kRedactedValueMarker);
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

/// `expectStatusList` wins over the legacy single value; with no expectation any status is
/// accepted.
[[nodiscard]] bool expectsStatus(const Operation& op, const int status) {
    if (!op.expectStatusList.empty()) {
        return std::ranges::find(op.expectStatusList, status) != op.expectStatusList.end();
    }
    if (op.expectStatus) {
        return status == *op.expectStatus;
    }
    return true;
}

[[nodiscard]] bool isIdempotentMethod(const HttpMethod method) noexcept {
    switch (method) {
        case HttpMethod::Get:
        case HttpMethod::Head:
        case HttpMethod::Options:
        case HttpMethod::Put:
        case HttpMethod::Delete:
            return true;
        case HttpMethod::Post:
        case HttpMethod::Patch:
            return false;
    }
    return false;
}

/// Idempotent methods, or a POST/PATCH with `retry.non_idempotent`. See RetryPolicy.
[[nodiscard]] bool mayRepeatAfterDelivery(const Operation& op) noexcept {
    return isIdempotentMethod(op.method) || op.retry.nonIdempotent;
}

[[nodiscard]] bool requestNeverReachedServer(const ErrorCode code) noexcept {
    return code == ErrorCode::NetworkDns || code == ErrorCode::NetworkConnect;
}

/// Register every loaded `{{secret.X}}` value and allowed `{{$.env.NAME}}` value for redaction.
void registerResolveSecrets(const ResolveContext& rctx, SecretRegistry& registry) {
    for (const auto& [name, value] : rctx.secrets) {
        registry.add(value);
    }
    for (const auto& name : rctx.allowedEnv) {
        // NOLINTNEXTLINE(concurrency-mt-unsafe) — same read VariableResolver makes.
        if (const char* value = std::getenv(name.c_str()); value != nullptr) {
            registry.add(value);
        }
    }
}

/// Items of one list extraction kept for agent-facing output.
constexpr std::size_t kExtractedListItems = 20;

/// Last field name of an extraction source (`$.data.access_token` → `access_token`).
[[nodiscard]] std::string_view sourceFieldName(std::string_view path) {
    while (!path.empty() && (path.back() == ']' || path.back() == '\'' || path.back() == '"')) {
        path.remove_suffix(1);
    }
    const auto cut = path.find_last_of(".['\"$");
    return cut == std::string_view::npos ? path : path.substr(cut + 1);
}

/// Secret when the variable name or its source field matches the sensitive-name policy.
[[nodiscard]] bool isSecretExtraction(const Extraction& ext) {
    return isSensitiveName(ext.variableName) || isSensitiveHeader(sourceFieldName(ext.sourcePath));
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
    // cancel() notifies under this mutex so a cancellable wait wakes at once.
    std::mutex cancelMutex;
    std::condition_variable cancelWake;

    // Operations that stored resource instances this run: the first store replaces earlier runs',
    // later ones (for_each) append.
    std::set<OperationId> producedThisRun;

    // Runs are serialized per engine (concurrent run() is unsupported), so a plain member is safe.
    bool captureResponseBodies{false};
    std::size_t failureExcerptBytes{kDefaultFailureExcerptBytes};
    // Keep each row's response body and extracted values for agent-facing output.
    bool recordResponses{false};
    std::size_t responseBodyBytes{kDefaultResponseBodyBytes};
    // The run prints to an agent (`RunOptions::agentOutput`): extra masking on. Off, the desktop
    // gets exactly the masking it always had.
    bool agentOutput{false};
    // Raw body bytes kept on rows so far this run (`kMaxRetainedBodyBytesPerRun`).
    std::size_t retainedBodyBytes{0};
    // `<resource>.<field>` references the project sends as credentials (`credentialReferences`).
    std::set<std::string> credentialRefs;
    // Where this run's secrets came from, to explain one that has no value.
    SecretSourceState secretSources{};

    // Secret values of the run in progress; set for the duration of run(), and emit() redacts
    // every event against it.
    const SecretRegistry* activeSecrets{nullptr};

    explicit Impl(Dependencies d) : deps(std::move(d)) {}

    /// Ids come from the open history store, unique across processes. Without a store a
    /// process-local counter suffices: the store refuses a RunStarted whose id belongs to a
    /// stored run.
    [[nodiscard]] RunId allocateRunId() {
        if (deps.history) {
            try {
                if (auto allocated = deps.history->allocateRunId(); allocated) {
                    return *allocated;
                }
                // NOLINTNEXTLINE(bugprone-empty-catch)
            } catch (...) {
            }
        }
        return RunId{nextRunId.fetch_add(1)};
    }

    [[nodiscard]] bool isCancelled(RunId runId) const noexcept {
        const auto cancelled = cancelledRunId.load(std::memory_order_acquire);
        return cancelled != 0 && cancelled == runId.value;
    }

    [[nodiscard]] CancelCheck cancelCheck(RunId runId) const noexcept {
        return CancelCheck{&cancelledRunId, runId.value};
    }

    /// Sleep for `delay` unless the run is cancelled first; true when cancelled.
    [[nodiscard]] bool waitUnlessCancelled(RunId runId, std::chrono::milliseconds delay) {
        std::unique_lock lock(cancelMutex);
        return cancelWake.wait_for(lock, delay, [&] { return isCancelled(runId); });
    }

    void storeInstances(RunContext& ctx,
                        const Operation& op,
                        std::vector<ResourceInstance> instances) {
        if (producedThisRun.insert(op.id).second) {
            ctx.replaceInstances(op.resource, op.id, std::move(instances));
            return;
        }
        for (auto& instance : instances) {
            instance.producer = op.id;
            ctx.appendInstance(op.resource, std::move(instance));
        }
    }

    // A secret-looking extraction, or one the project sends as a credential somewhere.
    // Desktop: as before, by name. Agent: also by credential-like names and by where the project
    // sends the value (`credentialRefs`).
    [[nodiscard]] bool isCredentialExtraction(const Operation& op, const Extraction& ext) const {
        if (!agentOutput) {
            return isSecretExtraction(ext);
        }
        return isSecretExtraction(ext) || isAgentSensitiveName(ext.variableName) ||
               isAgentSensitiveName(sourceFieldName(ext.sourcePath)) ||
               credentialRefs.contains(op.resource.value + "." + ext.variableName);
    }

    // With `recordResponses`, one extracted value for agent-facing output; a credential
    // extraction is only ever the marker.
    void keepExtracted(StepResult& row,
                       const Operation& op,
                       const Extraction& ext,
                       std::string name,
                       const std::string& value) const {
        if (recordResponses) {
            row.extracted.push_back(ExtractedValue{
                std::move(name),
                isCredentialExtraction(op, ext) ? std::string{kRedactedHeaderValue} : value,
                false});
        }
    }

    // The row's status, and with `recordResponses` its body, from the latest response.
    void keepResponse(StepResult& row, const HttpResponse& response, const RunContext& ctx) {
        row.httpStatus = response.status;
        if (agentOutput) {
            // A session cookie is a credential even if the run never sends it back.
            registerSetCookies(response.headers, ctx.secretRegistry());
        }
        if (recordResponses) {
            const bool budgetLeft = retainedBodyBytes < kMaxRetainedBodyBytesPerRun;
            keepResponseBody(row, response.body, budgetLeft ? responseBodyBytes : 0);
            retainedBodyBytes += row.response->body.size();
        }
    }

    /// Opt-in body payload for a `ResponseReceived` event: nullopt unless the run opted in, capped
    /// at `kMaxCapturedBodyBytes` UTF-8-safely.
    [[nodiscard]] std::optional<std::string> capturedBody(const std::string& body) const {
        if (!captureResponseBodies) {
            return std::nullopt;
        }
        if (body.size() > kMaxCapturedBodyBytes) {
            return codecs::truncateUtf8(body, kMaxCapturedBodyBytes);
        }
        return body;
    }

    void emit(const RunEvent& event) {
        RunEvent safeEvent{event};
        if (auto* prepared = std::get_if<RequestPrepared>(&safeEvent); prepared != nullptr) {
            // A prepared URL is display evidence, not replayable: any part may be sourced from
            // secrets, so all is opaque.
            prepared->url = sanitizeEventUrl(prepared->url);
        }
        // Value redaction of every registered secret in any string field; name-based masking at
        // emit sites is a second layer.
        if (activeSecrets != nullptr) {
            redactEvent(safeEvent, *activeSecrets);
        }
        // Persist before fanning out so the event survives a crashing subscriber. Best-effort: a
        // history failure (even a throw) must never abort the run.
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
                // Subscriber isolation: a misbehaving callback must not break the run for other
                // subscribers. NOLINTNEXTLINE(bugprone-empty-catch)
            } catch (...) {
            }
        }
    }

    /// After a refresh, rebuild the credential pre-resolved into the session's inject maps at login
    /// (keys mirror AuthStrategy.cpp: oauth2 → access_token, bearer → token, api_key → key). A
    /// refresh without that name leaves it as it was.
    static void refreshInjectedCredentials(const Actor& actor,
                                           const std::map<std::string, std::string>& refreshed,
                                           ActorSession& session) {
        const auto valueOf = [&](const std::string& key) -> const std::string* {
            const auto it = refreshed.find(key);
            return it == refreshed.end() ? nullptr : &it->second;
        };
        const bool isOAuth2 = actor.strategy == AuthStrategy::OAuth2ClientCredentials ||
                              actor.strategy == AuthStrategy::OAuth2Password;
        if (isOAuth2 || actor.strategy == AuthStrategy::Bearer) {
            if (const auto* token = valueOf(isOAuth2 ? "access_token" : "token")) {
                eraseHeaderCaseInsensitive(session.injectHeaders, "Authorization");
                session.injectHeaders["Authorization"] = "Bearer " + *token;
            }
            return;
        }
        if (actor.strategy == AuthStrategy::ApiKey) {
            const auto* key = valueOf("key");
            const auto locIt = actor.authConfig.find("location");
            const auto nameIt = actor.authConfig.find("name");
            if (key == nullptr || locIt == actor.authConfig.end() ||
                nameIt == actor.authConfig.end()) {
                return;
            }
            if (locIt->second == "header") {
                session.injectHeaders[nameIt->second] = *key;
            } else if (locIt->second == "query") {
                session.injectQueryParams[nameIt->second] = *key;
            }
        }
    }

    // A sign-in must not go out with a secret missing: that reads as a 401 from the API, not as
    // the missing secret it is (FR-11.6).
    [[nodiscard]] std::optional<ReqloomError> missingSignInSecret(
        const Actor& actor, const ResolveContext& rctx) const {
        for (const auto& name :
             referencedSecretNames(actorReferenceTemplates(actor), rctx.envVars)) {
            if (!rctx.secrets.contains(name)) {
                return ReqloomError{ErrorCode::VarUnresolved,
                                    ErrorClass::Resolution,
                                    "actor '" + actor.id.value +
                                        "' could not sign in: Variable: {{secret." + name +
                                        "}}\nLocation: sign-in of actor '" + actor.id.value +
                                        "'\nCause: " + missingSecretCause(name, secretSources)};
            }
        }
        return std::nullopt;
    }

    // Authenticate an actor if its session is not live; nullopt on success, else why sign-in
    // failed.
    std::optional<ReqloomError> ensureSession(const Actor& actor,
                                              RunContext& ctx,
                                              const ResolveContext& rctx,
                                              RunId runId,
                                              std::size_t stepIndex) {
        // Forward auth-flow events into Impl::emit under the parent step's runId/stepIndex.
        auto sink = [this](const RunEvent& ev) {
            this->emit(ev);
        };

        const auto* existing = ctx.session(actor.id);
        const bool live = existing != nullptr && existing->state == ActorSession::State::Live &&
                          std::chrono::steady_clock::now() < existing->expiresAt;
        if (!live) {
            if (auto missing = missingSignInSecret(actor, rctx)) {
                return missing;
            }
        }
        if ((existing != nullptr) && existing->state == ActorSession::State::Live) {
            const auto now = std::chrono::steady_clock::now();
            if (now < existing->expiresAt) {
                return std::nullopt;
            }

            if (actor.refresh) {
                AuthDependencies const refreshDeps{deps.http.get(),
                                                   &varResolver,
                                                   sink,
                                                   runId,
                                                   stepIndex,
                                                   captureResponseBodies,
                                                   failureExcerptBytes,
                                                   ExchangeKind::SessionRefresh,
                                                   cancelCheck(runId),
                                                   agentOutput};
                auto refreshed = runRefresh(actor, ctx, rctx, refreshDeps);
                if (refreshed) {
                    ActorSession updated = *existing;
                    refreshInjectedCredentials(actor, *refreshed, updated);
                    for (auto& [k, v] : *refreshed) {
                        updated.variables[k] = std::move(v);
                    }
                    updated.state = ActorSession::State::Live;
                    updated.expiresAt = std::chrono::steady_clock::now() +
                                        sessionLifetime(actor, updated.variables);
                    ctx.putSession(actor.id, std::move(updated));
                    emit(SessionRefreshed{runId,
                                          actor.id,
                                          SessionRefreshed::Trigger::Expiry,
                                          std::chrono::system_clock::now()});
                    return std::nullopt;
                }
            }
        }

        AuthDependencies authDeps{deps.http.get(),
                                  &varResolver,
                                  sink,
                                  runId,
                                  stepIndex,
                                  captureResponseBodies,
                                  failureExcerptBytes,
                                  ExchangeKind::SignIn,
                                  cancelCheck(runId),
                                  agentOutput};
        auto authenticator = selectAuthenticator(actor, std::move(authDeps));
        if (!authenticator) {
            return ReqloomError{ErrorCode::SessionRefreshFailed,
                                ErrorClass::Auth,
                                "actor '" + actor.id.value + "': unsupported auth strategy"};
        }

        auto outcome = authenticator->authenticate(actor, ctx, rctx);
        if (!outcome) {
            auto error = std::move(outcome.error());
            error.detail = "actor '" + actor.id.value + "' could not sign in: " + error.detail;
            return error;
        }

        ActorSession session = std::move(*outcome);
        session.state = ActorSession::State::Live;
        session.expiresAt =
            std::chrono::steady_clock::now() + sessionLifetime(actor, session.variables);
        ctx.putSession(actor.id, std::move(session));
        return std::nullopt;
    }

    // Polling phase: returns the FINAL poll response; errors PollFailPredicate, PollTimeout,
    // PollMaxAttemptsExceeded, SchemaInvalid.
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

        // Placeholders resolve once, before the first attempt, as values (PredicateTemplates.h).
        const auto substitute =
            [&](const std::string_view field,
                const std::string& predicate) -> std::expected<std::string, ReqloomError> {
            auto text = substitutePredicateTemplates(
                predicate, [&](std::string_view placeholder) -> std::optional<std::string> {
                    auto resolved = varResolver.resolve(placeholder, ctx, rctx);
                    if (!resolved.unresolvedOccurrences.empty()) {
                        return std::nullopt;
                    }
                    return std::move(resolved.output);
                });
            if (!text) {
                return std::unexpected(ReqloomError{ErrorCode::VarUnresolved,
                                                    ErrorClass::Resolution,
                                                    std::string{field} + ": unresolved variable " +
                                                        safeDiagnosticText(text.error())});
            }
            return std::move(*text);
        };
        const auto successText = substitute("poll_until.success_when", poll.successWhen);
        if (!successText) {
            return std::unexpected(successText.error());
        }
        auto successPredicate = evaluator.parse(*successText);
        if (!successPredicate) {
            return std::unexpected(
                ReqloomError{ErrorCode::SchemaInvalid,
                             ErrorClass::Schema,
                             "poll_until.success_when: " + successPredicate.error().detail});
        }

        std::optional<ParsedPredicate> failPredicate;
        if (poll.failWhen) {
            const auto failText = substitute("poll_until.fail_when", *poll.failWhen);
            if (!failText) {
                return std::unexpected(failText.error());
            }
            auto parsed = evaluator.parse(*failText);
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
        const auto& pollActorId = poll.actor ? *poll.actor : op.actor;
        if (!pollActorId.value.empty()) {
            auto it = project.actors.find(pollActorId);
            if (it == project.actors.end()) {
                return std::unexpected(
                    ReqloomError{ErrorCode::RefUndefined,
                                 ErrorClass::Schema,
                                 "poll_until: actor '" + pollActorId.value + "' is not defined"});
            }
            pollActor = &it->second;
        }

        if (pollActor != nullptr) {
            if (auto authError = ensureSession(*pollActor, ctx, rctx, runId, stepIndex)) {
                if (isCancelled(runId)) {
                    authError->code = ErrorCode::Cancelled;
                }
                authError->detail = "poll_until: " + authError->detail;
                return std::unexpected(std::move(*authError));
            }
        }

        const auto deadline = std::chrono::steady_clock::now() + poll.timeout;
        HttpResponse lastResponse;
        bool haveLastResponse = false;

        // Inline auth for poll requests, computed once (OAuth2 fetches its token a single time, not
        // per attempt).
        const std::optional<InlineAuth> effAuth = effectiveInlineAuth(op, project);
        std::map<std::string, std::string> inlineOAuth2Headers;
        if (effAuth && effAuth->type == InlineAuthType::OAuth2) {
            HttpRequest scratch;
            if (auto oauth2Err = applyInlineOAuth2(scratch, effAuth, ctx, rctx, runId, stepIndex)) {
                if (isCancelled(runId)) {
                    oauth2Err->code = ErrorCode::Cancelled;
                }
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
                                : std::string{kRedactedValueMarker};
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

                // Cookie jar emission, same priority as the parent op.
                if (!containsHeaderCaseInsensitive(req.headers, "Cookie")) {
                    if (auto header =
                            cookies::requestHeaderFor(ctx.jarCookies(pollActor->id), req.url);
                        !header.empty()) {
                        req.headers["Cookie"] = std::move(header);
                    }
                }
            }

            // Inline (actor-less) auth on the parent op also covers its poll requests.
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
                                             "poll_until: aws_sigv4 signing failed (" +
                                                 sigV4FailureReason(req, *session) + ")"});
                        }
                    }
                }
            }

            if (auto why = signInlineAuth(req, effAuth, ctx, rctx)) {
                return std::unexpected(
                    ReqloomError{ErrorCode::SessionRefreshFailed,
                                 ErrorClass::Auth,
                                 "poll_until: inline auth signing failed (" + *why + ")"});
            }

            if (agentOutput) {
                registerSentCredentials(req, ctx.secretRegistry());
            }
            emit(RequestPrepared{runId,
                                 stepIndex,
                                 req.method,
                                 req.url,
                                 headersToVector(maskHeaders(req.headers)),
                                 requestBodySize(req),
                                 std::chrono::system_clock::now(),
                                 ExchangeKind::PollAttempt});

            req.cancel = cancelCheck(runId);
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
                                  capturedBody(lastResponse.body),
                                  ExchangeKind::PollAttempt});

            // Absorb Set-Cookie from this poll so a follow-up op doesn't send a stale cookie.
            if (pollActor != nullptr) {
                cookies::absorbSetCookies(lastResponse.headers, pollActor->id, ctx, req.url);
            }

            // Each attempt is a timeline row; parse the body once and evaluate both predicates
            // against it.
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
            keepResponse(attemptRow, lastResponse, ctx);
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
            ctx.record(withoutEvidence(attemptRow));
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
            if (waitUnlessCancelled(runId, sleepFor)) {
                return std::unexpected(
                    ReqloomError{ErrorCode::Cancelled, ErrorClass::Run, "poll_until: cancelled"});
            }
        }

        return std::unexpected(ReqloomError{
            ErrorCode::PollMaxAttemptsExceeded,
            ErrorClass::Polling,
            haveLastResponse
                ? "poll_until: max_attempts (" + std::to_string(poll.maxAttempts) +
                      ") exceeded — last response: HTTP " + std::to_string(lastResponse.status)
                : "poll_until: max_attempts (" + std::to_string(poll.maxAttempts) + ") exceeded"});
    }

    // Apply an operation's actor-less inline auth; values resolve like any header. Runs after
    // actor/session injects so inline auth wins on Authorization.
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
                ctx.secretRegistry().add(token);
                eraseHeaderCaseInsensitive(req.headers, "Authorization");
                req.headers["Authorization"] = "Bearer " + token;
                break;
            }
            case InlineAuthType::Basic: {
                const auto user = varResolver.resolve(auth->username, ctx, rctx).output;
                const auto pass = varResolver.resolve(auth->password, ctx, rctx).output;
                ctx.secretRegistry().add(pass);
                ctx.secretRegistry().add(user + ":" + pass);
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
                ctx.secretRegistry().add(value);
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
                ctx.secretRegistry().add(secret);
                const auto payload = varResolver.resolve(auth->jwtPayload, ctx, rctx).output;
                // Only HS256/HS512 (empty = HS256); anything else fails the step rather than
                // signing with the wrong algorithm.
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
                ctx.secretRegistry().add(jwt);
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
                // AwsSigV4/OAuth1 sign per attempt (signInlineAuth); OAuth2 fetches a token; Mtls
                // is transport-level; Inherit is already resolved.
                break;
        }
        return std::nullopt;
    }

    // Per-request signing for inline AWS SigV4 / OAuth1 via the actor-path signers on a transient
    // session. Returns why signing failed (req untouched) or nullopt.
    [[nodiscard]] std::optional<std::string> signInlineAuth(HttpRequest& req,
                                                            const std::optional<InlineAuth>& auth,
                                                            const RunContext& ctx,
                                                            const ResolveContext& rctx) const {
        if (!auth) {
            return std::nullopt;
        }
        const auto resolve = [&](const std::string& value) {
            return varResolver.resolve(value, ctx, rctx).output;
        };
        const auto resolveSecret = [&](const std::string& value) {
            auto resolved = resolve(value);
            ctx.secretRegistry().add(resolved);
            return resolved;
        };
        switch (auth->type) {
            case InlineAuthType::AwsSigV4: {
                ActorSession session;
                session.variables["access_key"] = resolveSecret(auth->awsAccessKey);
                session.variables["secret_key"] = resolveSecret(auth->awsSecretKey);
                session.variables["region"] = resolve(auth->awsRegion);
                session.variables["service"] = resolve(auth->awsService);
                if (!auth->awsSessionToken.empty()) {
                    session.variables["session_token"] = resolveSecret(auth->awsSessionToken);
                }
                eraseHeaderCaseInsensitive(req.headers, "Authorization");
                if (signSigV4Request(req, session)) {
                    return std::nullopt;
                }
                return sigV4FailureReason(req, session);
            }
            case InlineAuthType::OAuth1: {
                ActorSession session;
                session.variables["consumer_key"] = resolveSecret(auth->oauthConsumerKey);
                session.variables["consumer_secret"] = resolveSecret(auth->oauthConsumerSecret);
                session.variables["token"] = resolveSecret(auth->oauthToken);
                session.variables["token_secret"] = resolveSecret(auth->oauthTokenSecret);
                eraseHeaderCaseInsensitive(req.headers, "Authorization");
                if (signOAuth1Request(req, session)) {
                    return std::nullopt;
                }
                return std::string{"missing credentials or malformed URL"};
            }
            case InlineAuthType::None:
            case InlineAuthType::Bearer:
            case InlineAuthType::Basic:
            case InlineAuthType::ApiKey:
            case InlineAuthType::OAuth2:
            case InlineAuthType::Jwt:
            case InlineAuthType::Mtls:
            case InlineAuthType::Inherit:
                return std::nullopt;  // non-signing types handled elsewhere
        }
        return std::nullopt;
    }

    /// Why SigV4 signing of `req` failed: its own refusal reason, else the credential / URL causes.
    [[nodiscard]] static std::string sigV4FailureReason(const HttpRequest& req,
                                                        const ActorSession& session) {
        return sigV4Refusal(req, session)
            .value_or("missing access_key/secret_key/region/service or malformed URL");
    }

    // Resolve effective inline auth: Inherit takes the project default; a nested Inherit or absent
    // default means no auth.
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

    // Mutual TLS: stamp client cert/key onto the request transport; composes with header auth.
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
            ctx.secretRegistry().add(*req.transport.clientKeyPassword);
        }
        if (!auth->mtlsCaCertPath.empty()) {
            req.transport.caBundlePath =
                varResolver.resolve(auth->mtlsCaCertPath, ctx, rctx).output;
        }
    }

    // OAuth 2.0 client credentials: fetch a token via a transient actor and the OAuth2
    // authenticator, then inject Bearer. The token is cached in the session store keyed by
    // token_url/client_id/username/scope and honors `expires_in`, so ops sharing a config reuse it.
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

        // Authorization Code (PKCE) is interactive: the desktop stores the token on the op; the
        // engine only injects it.
        if (auth->oauth2GrantType == "authorization_code") {
            const auto token = varResolver.resolve(auth->oauth2AccessToken, ctx, rctx).output;
            ctx.secretRegistry().add(token);
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

        // Hashed: a resolved value can carry a secret, and the key is stored as an actor id.
        std::string cacheKeyMaterial = auth->oauth2GrantType;
        for (const auto* field : {&auth->oauth2TokenUrl,
                                  &auth->oauth2ClientId,
                                  &auth->username,
                                  &auth->oauth2Scope,
                                  &auth->oauth2ClientAuth}) {
            const auto resolved = varResolver.resolve(*field, ctx, rctx).output;
            cacheKeyMaterial += '\n' + std::to_string(resolved.size()) + ':' + resolved;
        }
        const ActorId cacheId{"__inline_oauth2:" +
                              codecs::hexEncode(crypto::sha256(cacheKeyMaterial))};

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
        // Only non-interactive grants run headlessly: "password", else client_credentials.
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
        AuthDependencies authDeps{deps.http.get(),
                                  &varResolver,
                                  sink,
                                  runId,
                                  stepIndex,
                                  captureResponseBodies,
                                  failureExcerptBytes,
                                  ExchangeKind::InlineAuthToken,
                                  cancelCheck(runId),
                                  agentOutput};
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

        // Lifetime from `expires_in` (default 5 min) minus a 30s margin.
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

    // Execute one operation step. pollAttemptRows is an out-parameter: a StepResult can't carry
    // vector<StepResult> without recursing.
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
        if (isCancelled(runId)) {
            result.status = StepResult::Status::Cancelled;
            result.error = ErrorCode::Cancelled;
            return result;
        }

        constexpr std::size_t kMaxDiagnosticsPerStep{128};
        bool foundUnresolved{};
        bool ignoreCurrentActorUnresolved{};
        std::optional<std::pair<std::string, std::string>> firstUnresolved{};

        const auto unresolvedVariableDetail = [this, &rctx, &ctx](
                                                  const std::string_view safeVariable,
                                                  const std::string_view location) {
            auto reason = envBuiltinFailureReason(safeVariable, rctx);
            if (!reason) {
                reason = unsetFieldReason(ctx, safeVariable);
            }
            if (!reason) {
                reason = unresolvedSecretReason(safeVariable, rctx, secretSources);
            }
            return std::string{"Variable: {{"} + std::string{safeVariable} +
                   "}}\nLocation: " + std::string{location} + "\nCause: " +
                   reason.value_or(
                       "No usable value was available in the current run when "
                       "this request was prepared.");
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
                // A `$.env.NAME` token names a variable, never a value, so showing it is safe.
                const bool showToken =
                    exposeToken || envBuiltinFailureReason(occurrence.token, rctx).has_value();
                firstUnresolved = std::pair{showToken ? safeDiagnosticText(occurrence.token)
                                                      : std::string{kRedactedValueMarker},
                                            std::string{location}};
            }
            // Bound untrusted template amplification.
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

        const std::optional<InlineAuth> effAuth = effectiveInlineAuth(op, project);
        const auto failPreparation = [&](const ErrorCode code, std::string detail) {
            // A cancel stops a sign-in or token request in flight, which then fails preparation.
            const bool cancelled = isCancelled(runId);
            result.status = cancelled ? StepResult::Status::Cancelled : StepResult::Status::Failed;
            result.error = cancelled ? ErrorCode::Cancelled : code;
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
                    if (auto header = cookies::requestHeaderFor(ctx.jarCookies(op.actor), req.url);
                        !header.empty()) {
                        req.headers["Cookie"] = std::move(header);
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

            FormFields resolvedFormFields{};
            if (op.bodyTemplate) {
                auto resolved =
                    resolveRequestBody(varResolver, *op.bodyTemplate, req.headers, ctx, rctx);
                addValueDiagnostics(resolved, VariableUseKind::Body, {}, "Request body");
                req.body = resolved.output;
            } else if (op.bodyForm) {
                for (const auto& [key, value] : *op.bodyForm) {
                    // File or text is decided from the template, never from the substituted value.
                    const auto field = classifyFormTemplate(value);
                    auto resolved = varResolver.resolve(field.rest, ctx, rctx);
                    addValueDiagnostics(
                        resolved,
                        VariableUseKind::FormField,
                        key,
                        std::string{"Form field \""} + safeDiagnosticText(key) + '"');
                    resolvedFormFields[key] = FormField{field.kind, resolved.output};
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

            // Side-effectful OAuth acquisition comes after request-template preflight.
            if (auto authErr = applyInlineAuth(req, effAuth, ctx, rctx)) {
                return failPreparation(authErr->code, authErr->detail);
            }

            if (op.bodyTemplate) {
                if (!containsHeaderCaseInsensitive(req.headers, "Content-Type")) {
                    req.headers["Content-Type"] = "application/json";
                }
            } else if (op.bodyForm) {
                const bool routeMultipart = wantsMultipart(op.headers, resolvedFormFields);
                auto formBody = buildFormBody(resolvedFormFields, routeMultipart, project.rootDir);
                if (!formBody) {
                    return failPreparation(formBody.error().code, formBody.error().detail);
                }
                if (auto* multipart = std::get_if<MultipartBody>(&*formBody)) {
                    req.multipart = std::move(multipart->parts);
                    // libcurl writes Content-Type with the boundary; drop a user-supplied one so it
                    // isn't sent twice.
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

            // Hooks rerun after a refresh because their output may depend on freshly resolved
            // values.
            if (op.preRequestScript && deps.hooks) {
                auto hookContext =
                    buildHookContext(req, ctx, rctx, op, project, *op.preRequestScript);
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

        // Diagnose target templates before authentication can issue HTTP; only the current actor's
        // own session fields may be left to authentication.
        ignoreCurrentActorUnresolved = true;
        if (!prepareRequest(true)) {
            return result;
        }
        ignoreCurrentActorUnresolved = false;
        foundUnresolved = false;
        firstUnresolved.reset();

        if (!op.actor.value.empty()) {
            const auto actorIt = project.actors.find(op.actor);
            if (actorIt == project.actors.end()) {
                failPreparation(ErrorCode::RefUndefined,
                                "Operation '" + op.id.value + "' names actor '" + op.actor.value +
                                    "', which is not defined");
                return result;
            }
            if (auto authError = ensureSession(actorIt->second, ctx, rctx, runId, stepIndex)) {
                // A missing secret keeps its own code; every other sign-in failure is an auth one.
                failPreparation(authError->code == ErrorCode::VarUnresolved
                                    ? ErrorCode::VarUnresolved
                                    : ErrorCode::SessionRefreshFailed,
                                std::move(authError->detail));
                return result;
            }
        }

        if (!prepareRequest(false)) {
            return result;
        }

        const int maxAttempts = std::max(op.retry.maxAttempts, 1);
        std::optional<HttpResponse> httpResp;
        ReqloomError lastError{};
        int attemptCount = 0;
        const auto backoff = [&](const int attempt) {
            // Exponential backoff; the shift is capped at 20 to avoid signed-overflow UB.
            const auto shift = std::min(attempt, 20);
            auto delay = op.retry.baseBackoff * (std::uint32_t{1} << shift);
            if (delay > op.retry.maxBackoff) {
                delay = op.retry.maxBackoff;
            }
            // The next attempt's cancellation check ends the step.
            (void)waitUnlessCancelled(runId, delay);
        };

        for (int attempt = 0; attempt < maxAttempts; ++attempt) {
            ++attemptCount;
            if (isCancelled(runId)) {
                result.status = StepResult::Status::Cancelled;
                result.error = ErrorCode::Cancelled;
                result.attempts = attemptCount;
                return result;
            }

            // Sign inside the retry loop so each attempt gets a fresh nonce/timestamp (OAuth 1.0a /
            // SigV4).
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
                            result.detail = "aws_sigv4 signing failed (" +
                                            sigV4FailureReason(req, *session) + ")";
                            result.attempts = attemptCount;
                            return result;
                        }
                    }
                }
            }

            if (auto why = signInlineAuth(req, effAuth, ctx, rctx)) {
                result.status = StepResult::Status::Failed;
                result.error = ErrorCode::SessionRefreshFailed;
                result.detail = "inline auth signing failed (" + *why + ")";
                result.attempts = attemptCount;
                return result;
            }

            if (agentOutput) {
                registerSentCredentials(req, ctx.secretRegistry());
            }
            emit(RequestPrepared{runId,
                                 stepIndex,
                                 req.method,
                                 req.url,
                                 headersToVector(maskHeaders(req.headers)),
                                 requestBodySize(req),
                                 std::chrono::system_clock::now(),
                                 ExchangeKind::Operation});

            req.cancel = cancelCheck(runId);
            auto resp = deps.http->send(req);
            if (resp) {
                httpResp = std::move(*resp);
                keepResponse(result, *httpResp, ctx);
                emit(ResponseReceived{runId,
                                      stepIndex,
                                      httpResp->status,
                                      maskHeaders(httpResp->headers),
                                      httpResp->body.size(),
                                      httpResp->elapsed,
                                      std::chrono::system_clock::now(),
                                      capturedBody(httpResp->body),
                                      ExchangeKind::Operation});
                // Absorb Set-Cookie at receive time so a post_response hook sees the current jar.
                if (!op.actor.value.empty()) {
                    cookies::absorbSetCookies(httpResp->headers, op.actor, ctx, req.url);
                }
                // An unexpected 5xx reached the server, so only safe-to-repeat methods are re-sent.
                const bool retry5xx = httpResp->status >= 500 &&
                                      !expectsStatus(op, httpResp->status) &&
                                      mayRepeatAfterDelivery(op) && attempt + 1 < maxAttempts;
                if (!retry5xx) {
                    break;
                }
                backoff(attempt);
                continue;
            }
            lastError = resp.error();
            if (lastError.code == ErrorCode::Cancelled) {
                result.status = StepResult::Status::Cancelled;
                result.error = ErrorCode::Cancelled;
                result.attempts = attemptCount;
                return result;
            }
            // POST/PATCH without `retry.non_idempotent` are re-sent only when the request provably
            // never reached the server.
            const bool safeToResend =
                mayRepeatAfterDelivery(op) || requestNeverReachedServer(lastError.code);
            if (!isRetryable(lastError.code) || !safeToResend || attempt + 1 >= maxAttempts) {
                result.status = StepResult::Status::Failed;
                result.error = lastError.code;
                result.attempts = attemptCount;
                return result;
            }
            backoff(attempt);
        }

        result.attempts = attemptCount;

        // 401 recovery: re-authenticate once and retry, only when the op has an actor, the response
        // is a real 401, and `expect_status:` doesn't include 401. Emits
        // SessionRefreshed{Trigger::Unauthorized}.
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
                const auto cancelledStep = [&] {
                    result.status = StepResult::Status::Cancelled;
                    result.error = ErrorCode::Cancelled;
                    return result;
                };
                if (isCancelled(runId)) {
                    return cancelledStep();
                }
                ctx.invalidateSession(op.actor);
                if (!ensureSession(actorIt->second, ctx, rctx, runId, stepIndex)) {
                    if (isCancelled(runId)) {
                        return cancelledStep();
                    }
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
                                result.detail = "aws_sigv4 signing failed after session refresh (" +
                                                sigV4FailureReason(req, *session) + ")";
                                return result;
                            }
                        }
                    }
                    if (auto why = signInlineAuth(req, effAuth, ctx, rctx)) {
                        result.status = StepResult::Status::Failed;
                        result.error = ErrorCode::SessionRefreshFailed;
                        result.detail =
                            "inline auth signing failed after session refresh (" + *why + ")";
                        return result;
                    }

                    if (agentOutput) {
                        registerSentCredentials(req, ctx.secretRegistry());
                    }
                    emit(RequestPrepared{runId,
                                         stepIndex,
                                         req.method,
                                         req.url,
                                         headersToVector(maskHeaders(req.headers)),
                                         requestBodySize(req),
                                         std::chrono::system_clock::now(),
                                         ExchangeKind::Operation});

                    req.cancel = cancelCheck(runId);
                    auto retryResp = deps.http->send(req);
                    ++attemptCount;
                    result.attempts = attemptCount;
                    if (retryResp) {
                        httpResp = std::move(*retryResp);
                        keepResponse(result, *httpResp, ctx);
                        emit(ResponseReceived{runId,
                                              stepIndex,
                                              httpResp->status,
                                              maskHeaders(httpResp->headers),
                                              httpResp->body.size(),
                                              httpResp->elapsed,
                                              std::chrono::system_clock::now(),
                                              capturedBody(httpResp->body),
                                              ExchangeKind::Operation});
                        cookies::absorbSetCookies(httpResp->headers, op.actor, ctx, req.url);
                    } else {
                        // Surface the retry's network error rather than the original 401.
                        result.status = retryResp.error().code == ErrorCode::Cancelled
                                            ? StepResult::Status::Cancelled
                                            : StepResult::Status::Failed;
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

        const bool statusMatches = !httpResp || expectsStatus(op, httpResp->status);

        if (!statusMatches) {
            result.status = StepResult::Status::Failed;
            result.error = (httpResp->status >= 500) ? ErrorCode::Http5xx : ErrorCode::Http4xx;
            std::string expected{};
            if (!op.expectStatusList.empty()) {
                for (const int status : op.expectStatusList) {
                    expected += (expected.empty() ? "" : ", ") + std::to_string(status);
                }
            } else if (op.expectStatus) {
                expected = std::to_string(*op.expectStatus);
            }
            // The detail reaches events, history, UI and CLI: the quoted body is masked, and
            // omitted if it cannot be made safe.
            result.detail =
                "HTTP " + std::to_string(httpResp->status) + " (expected " + expected + ")";
            const auto& show = op.failureMessageShow.value_or(project.failureMessageShow);
            const auto profile = agentOutput ? MaskProfile::Agent : MaskProfile::Display;
            auto excerpt = selectedBodyExcerpt(
                httpResp->body, show, &ctx.secretRegistry(), failureExcerptBytes, profile);
            if (!excerpt) {
                excerpt = safeBodyExcerpt(
                    httpResp->body, &ctx.secretRegistry(), failureExcerptBytes, profile);
            }
            if (excerpt) {
                result.detail += " — " + *excerpt;
            }
            auto elapsed = std::chrono::steady_clock::now() - startTime;
            result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
            return result;
        }

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
                result.status = pollResult.error().code == ErrorCode::Cancelled
                                    ? StepResult::Status::Cancelled
                                    : StepResult::Status::Failed;
                result.error = pollResult.error().code;
                result.detail = pollResult.error().detail;
                auto elapsed = std::chrono::steady_clock::now() - startTime;
                result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
                return result;
            }
            httpResp = std::move(*pollResult);
        }

        // post_response hook runs after the final (post-poll) response and before extraction; its
        // mutations are seen by extractions.
        if (httpResp && op.postResponseScript && deps.hooks) {
            auto hctx = buildHookContext(req, ctx, rctx, op, project, *op.postResponseScript);
            HookResponseView respView;
            respView.status = httpResp->status;
            respView.headers = headersToMap(httpResp->headers);
            respView.body = httpResp->body;
            const auto exposedHeaders = respView.headers;
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
                httpResp->headers = mergeHookHeaders(
                    httpResp->headers, exposedHeaders, outcome->mutatedResponse->headers);
                httpResp->body = std::move(outcome->mutatedResponse->body);
            }
        }

        if (httpResp && !op.extractions.empty()) {
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
            std::set<std::string> nullVariables;
            std::set<std::string> missingVariables;
            if (!scalarExts.empty()) {
                auto detailed = extractFromResponseDetailed(
                    op.id, httpResp->body, httpResp->status, httpResp->headers, scalarExts);
                if (!detailed) {
                    result.status = StepResult::Status::Failed;
                    result.error = detailed.error().code;
                    result.detail = detailed.error().detail;
                    auto elapsed = std::chrono::steady_clock::now() - startTime;
                    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
                    return result;
                }
                // Register secret-looking extractions (full values) before any event can carry
                // them.
                for (const auto& ext : scalarExts) {
                    const auto found = detailed->values.find(ext.variableName);
                    if (found != detailed->values.end() && isCredentialExtraction(op, ext)) {
                        ctx.secretRegistry().add(found->second);
                    }
                    if (found != detailed->values.end()) {
                        keepExtracted(result, op, ext, ext.variableName, found->second);
                    }
                }

                // Trace every extraction outcome, misses included, so the timeline shows nulls and
                // missing fields.
                std::optional<std::string> firstMiss;
                for (auto& t : detailed->traces) {
                    const bool isMissLike = t.outcome == ExtractionTrace::Outcome::Missing ||
                                            t.outcome == ExtractionTrace::Outcome::InvalidPattern;
                    if (!firstMiss && isMissLike) {
                        firstMiss = t.variableName;
                    }
                    if (t.outcome == ExtractionTrace::Outcome::Null) {
                        nullVariables.insert(t.variableName);
                    } else if (t.outcome != ExtractionTrace::Outcome::Resolved) {
                        missingVariables.insert(t.variableName);
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
                            // RunContext keeps the real value; the event copy is masked when the
                            // name looks secret.
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
                    // Record the miss so a later reference cannot use this op's earlier value.
                    std::vector<ResourceInstance> latest(1);
                    latest.front().variables = std::move(detailed->values);
                    latest.front().nullVariables = std::move(nullVariables);
                    latest.front().missingVariables = std::move(missingVariables);
                    storeInstances(ctx, op, std::move(latest));
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
                if (!scalarValues.empty() || !nullVariables.empty()) {
                    ResourceInstance instance;
                    instance.variables = std::move(scalarValues);
                    instance.nullVariables = std::move(nullVariables);

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
                    std::vector<ResourceInstance> latest;
                    latest.push_back(std::move(instance));
                    storeInstances(ctx, op, std::move(latest));
                }
            } else {
                // List extraction: each `[*]` path yields a vector; one instance per item, scalars
                // broadcast.
                std::size_t count = 0;
                std::vector<std::pair<std::string, std::vector<std::string>>> lists;
                std::optional<std::string> emptyList;
                for (const auto& ext : listExts) {
                    auto values = extractValues(httpResp->body, ext.sourcePath);
                    if (isCredentialExtraction(op, ext)) {
                        for (const auto& value : values) {
                            ctx.secretRegistry().add(value);
                        }
                    }
                    for (std::size_t k = 0; k < std::min(values.size(), kExtractedListItems); ++k) {
                        keepExtracted(result,
                                      op,
                                      ext,
                                      ext.variableName + "[" + std::to_string(k) + "]",
                                      values[k]);
                    }
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
                    storeInstances(ctx, op, {});
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

                std::vector<ResourceInstance> items;
                items.reserve(count);
                for (std::size_t k = 0; k < count; ++k) {
                    ResourceInstance instance;
                    instance.variables = scalarValues;  // broadcast scalars
                    instance.nullVariables = nullVariables;
                    for (const auto& [var, values] : lists) {
                        if (k < values.size()) {
                            instance.variables[var] = values[k];
                        }
                    }
                    items.push_back(std::move(instance));
                }
                storeInstances(ctx, op, std::move(items));
            }
        }

        // Response assertions: evaluate each predicate against the final response and record every
        // outcome; the first failure fails the step.
        if (httpResp && !op.assertions.empty()) {
            const PredicateEvaluator assertionEval;
            const auto parsedBody = assertionEval.parseBody(httpResp->body);
            std::optional<std::string> firstFail;
            std::string firstFailParseError;
            std::string firstFailUnresolved;
            for (const auto& assertion : op.assertions) {
                AssertionResult ar;
                ar.expr = assertion.expr;
                ar.name = assertion.name.value_or(assertion.expr);
                const std::string location = "Assertion \"" + safeDiagnosticText(ar.name) + '"';
                // Events and details carry `assertion.expr`; the substituted text stays here.
                const auto substituted = substitutePredicateTemplates(
                    assertion.expr,
                    [&](std::string_view placeholder) -> std::optional<std::string> {
                        auto resolved = varResolver.resolve(placeholder, ctx, rctx);
                        if (!resolved.unresolvedOccurrences.empty()) {
                            addValueDiagnostics(resolved, VariableUseKind::Unknown, {}, location);
                            return std::nullopt;
                        }
                        return std::move(resolved.output);
                    });
                const auto parsed = substituted ? assertionEval.parse(*substituted)
                                                : std::expected<ParsedPredicate, ReqloomError>{
                                                      std::unexpect, ReqloomError{}};
                if (parsed) {
                    auto explained = assertionEval.explain(
                        *parsed, parsedBody, httpResp->status, &ctx.secretRegistry());
                    ar.passed = explained.value == PredicateValue::True;
                    ar.detail = std::move(explained.detail);
                } else if (!substituted && firstUnresolved) {
                    ar.detail =
                        unresolvedVariableDetail(firstUnresolved->first, firstUnresolved->second);
                } else if (substituted) {
                    ar.detail = parsed.error().detail;
                }
                if (!ar.passed && !firstFail) {
                    firstFail = ar.name;
                    if (!substituted && firstUnresolved) {
                        firstFailUnresolved = ar.detail;
                    } else if (!parsed) {
                        firstFailParseError = parsed.error().detail;
                    }
                }
                emit(AssertionCompleted{runId,
                                        stepIndex,
                                        op.id,
                                        ar.name,
                                        ar.expr,
                                        ar.passed,
                                        std::chrono::system_clock::now(),
                                        ar.detail});
                result.assertions.push_back(std::move(ar));
            }
            if (firstFail && !firstFailUnresolved.empty()) {
                result.status = StepResult::Status::Failed;
                result.error = ErrorCode::VarUnresolved;
                result.detail = std::move(firstFailUnresolved);
                auto elapsed = std::chrono::steady_clock::now() - startTime;
                result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
                return result;
            }
            if (firstFail) {
                result.status = StepResult::Status::Failed;
                result.error = ErrorCode::AssertionFailed;
                result.detail = firstFailParseError.empty()
                                    ? "assertion failed: " + *firstFail + " for " + op.id.value
                                    : "assertion '" + *firstFail +
                                          "' is not a valid predicate for " + op.id.value + ": " +
                                          firstFailParseError;
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
    impl_->producedThisRun.clear();
    impl_->captureResponseBodies = options.captureResponseBodies;
    impl_->recordResponses = options.recordResponses;
    // Responses on StepResult are agent-facing, so recording them turns agent masking on.
    impl_->agentOutput = options.agentOutput || options.recordResponses;
    impl_->retainedBodyBytes = 0;
    impl_->responseBodyBytes = std::min(options.responseBodyBytes, kMaxFailureExcerptBytes);
    impl_->credentialRefs =
        impl_->agentOutput ? credentialReferences(project) : std::set<std::string>{};
    if (impl_->agentOutput) {
        ctx.secretRegistry().enableJsonEscapedSpellings();
    }
    impl_->failureExcerptBytes = std::min(options.failureExcerptBytes, kMaxFailureExcerptBytes);
    const auto runStart = std::chrono::steady_clock::now();
    impl_->activeSecrets = &ctx.secretRegistry();
    struct ActiveSecretsReset {
        const SecretRegistry*& slot;
        ~ActiveSecretsReset() { slot = nullptr; }
    } const activeSecretsReset{impl_->activeSecrets};

    if (options.resetExtractions) {
        ctx.clearExtractions();
    }
    const std::size_t extractionTraceStart{ctx.extractionTrace().size()};
    if (options.resetSessions) {
        // Inline-OAuth2 token caches live under synthetic `__inline_oauth2:` ids, so clear all.
        ctx.clearSessions();
    }

    auto chainResult = impl_->resolver.resolve(project, target);
    if (!chainResult) {
        return std::unexpected(chainResult.error());
    }

    const auto& chain = *chainResult;

    ResolveContext rctx;
    auto envName = options.environment.empty() ? project.defaultEnvironment : options.environment;
    if (project.environments.contains(envName)) {
        rctx.envVars = project.environments.at(envName);
    }
    // Resolve per-env transport overrides once at run start; every request sees them via
    // rctx.transport.
    if (auto it = project.transport.find(envName); it != project.transport.end()) {
        rctx.transport = it->second;
    }
    rctx.allowedEnv.insert(project.allowedEnv.begin(), project.allowedEnv.end());

    // Pre-load only the secrets the project references from the OS keychain (never a bulk dump). A
    // missing key stays unset (VarUnresolved later); a backend failure aborts with
    // SecretAccessFailed. Skipped on dry runs: a preview must not trigger a keychain prompt. The
    // run id is announced first: a keychain prompt can block, and cancel() needs a target.
    const auto runId = impl_->allocateRunId();
    impl_->emit(RunStarted{runId, target, chain.size(), envName, std::chrono::system_clock::now()});
    const auto emitRunEnded = [&](const RunOutcome outcome) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - runStart);
        impl_->emit(RunEnded{runId, outcome, elapsed, std::chrono::system_clock::now()});
    };
    const auto failBeforeFirstStep = [&](ReqloomError error) {
        emitRunEnded(RunOutcome::Failed);
        return std::unexpected(std::move(error));
    };
    // `REQLOOM_SECRET_<NAME>` first (FR-11.5): a hit needs no keychain, project id or trust
    // check. Only the names left go to the keychain, and only when it exists.
    impl_->secretSources = SecretSourceState{};
    impl_->secretSources.environmentEnabled = options.secretsFromEnvironment;
    if (!options.dryRun) {
        auto secretNames = DependencyResolver::collectSecretReferences(project);
        appendHookScriptSecrets(project, chain, secretNames);
        if (options.secretsFromEnvironment) {
            // `api_key` and `API_KEY` would silently get one value: refuse instead.
            if (auto shared = sharedEnvironmentVariable(secretNames)) {
                return failBeforeFirstStep(
                    ReqloomError{ErrorCode::SchemaInvalid,
                                 ErrorClass::Schema,
                                 "secrets " + *shared +
                                     ", so both would get the same value; rename one of them"});
            }
        }
        std::vector<std::string> keychainNames{};
        for (const auto& name : secretNames) {
            if (!options.secretsFromEnvironment) {
                keychainNames.push_back(name);
            } else if (auto value = readSecretFromEnvironment(name, impl_->secretSources)) {
                rctx.secrets[name] = std::move(*value);
            } else {
                keychainNames.push_back(name);
            }
        }
        impl_->secretSources.backendAvailable =
            impl_->deps.secrets && impl_->deps.secrets->available();
        if (!impl_->secretSources.backendAvailable) {
            keychainNames.clear();
        }
        // A copied or hostile project may claim another project's id; refuse before any read.
        if (!keychainNames.empty()) {
            if (auto trusted = checkProjectFolder(impl_->deps.trust.get(), project); !trusted) {
                return failBeforeFirstStep(trusted.error());
            }
        }
        for (const auto& name : keychainNames) {
            auto key = projectSecretKey(project, name);
            if (!key) {
                return failBeforeFirstStep(key.error());
            }
            auto value = impl_->deps.secrets->read(*key);
            if (impl_->isCancelled(runId)) {
                RunResult cancelled;
                cancelled.runId = runId;
                cancelled.outcome = RunOutcome::Cancelled;
                for (std::size_t j = 0; j < chain.size(); ++j) {
                    StepResult row;
                    row.op = chain[j];
                    row.status = StepResult::Status::Cancelled;
                    cancelled.steps.push_back(std::move(row));
                    impl_->emit(
                        StepCancelled{runId, j, chain[j], std::chrono::system_clock::now()});
                }
                emitRunEnded(RunOutcome::Cancelled);
                return cancelled;
            }
            if (!value) {
                const auto variable =
                    options.secretsFromEnvironment ? secretEnvironmentVariable(name) : std::nullopt;
                return failBeforeFirstStep(ReqloomError{
                    ErrorCode::SecretAccessFailed,
                    ErrorClass::Auth,
                    "secret store: failed to read '" + name + "': " + value.error().detail +
                        (variable ? " To supply it without the keychain, set " + *variable + "."
                                  : std::string{})});
            }
            // A read can find out only while reading that no keychain is reachable (no session
            // bus, no event loop); the run then says so. Per read, so the next run tries again.
            if (impl_->deps.secrets->lastReadUnreachable()) {
                impl_->secretSources.backendAvailable = false;
            }
            if (value->has_value()) {
                rctx.secrets[name] = std::move(**value);
            }
        }
    }

    registerResolveSecrets(rctx, ctx.secretRegistry());

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
                    for (auto inst = instances.rbegin(); inst != instances.rend(); ++inst) {
                        if (inst->variables.contains(ext.variableName)) {
                            found = true;
                            break;
                        }
                        if (inst->isUnset(ext.variableName)) {
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
                    ctx.record(skipResult);
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
            // Data-driven fan-out: one run per instance of the `over` resource, binding
            // `{{<over>.field}}`; rows carry forEachIndex and group under this step.
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
                    redactStepResult(iter, ctx.secretRegistry());
                    for (auto& row : iterPollRows) {
                        redactStepResult(row, ctx.secretRegistry());
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
                    ctx.record(withoutEvidence(iter));
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
            redactStepResult(stepResult, ctx.secretRegistry());
            // Per-attempt rows precede the parent step row so renderers can
            // group them under the operation that owned the poll loop.
            for (auto& row : pollAttemptRows) {
                redactStepResult(row, ctx.secretRegistry());
                result.steps.push_back(std::move(row));
            }
            result.steps.push_back(stepResult);
            ctx.record(withoutEvidence(stepResult));
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

    // Mask with every secret the run learned (credentials sent in later steps included), then cut.
    if (impl_->agentOutput) {
        finalizeStepEvidence(
            result.steps, ctx, options.recordResponses ? options.responseBodyBytes : 0);
    }
    emitRunEnded(result.outcome);
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
    {
        const std::lock_guard lock(impl_->cancelMutex);
        impl_->cancelledRunId.store(run.value, std::memory_order_release);
    }
    impl_->cancelWake.notify_all();
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
