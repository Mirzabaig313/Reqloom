// Authenticator — strategy dispatch and concrete implementations.
#include "AuthStrategy.h"
#include "Cookies.h"
#include "HeaderMasking.h"
#include "JsonExtraction.h"
#include "SecretRedaction.h"
#include "StepEvidence.h"

#include "../domain/Codecs.h"
#include "../domain/JsonBodyTemplate.h"
#include "../domain/SecretRegistry.h"
#include "../domain/VariableResolver.h"
#include "../infrastructure/http/HttpClient.h"
#include "../infrastructure/util/Crypto.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <system_error>

#include <reqloom/engine/Actor.h>
#include <reqloom/engine/Events.h>
#include <reqloom/engine/ExecutionEngine.h>  // kMaxCapturedBodyBytes

#include <nlohmann/json.hpp>

#include <chrono>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace reqloom::engine {

namespace {

using namespace codecs;
using json = nlohmann::json;

[[nodiscard]] std::vector<std::pair<std::string, std::string>> snapshotMaskedRequestHeaders(
    const HttpRequest& req) {
    auto masked = maskHeaders(req.headers);
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(masked.size());
    for (const auto& [k, v] : masked) {
        out.emplace_back(k, v);
    }
    return out;
}

[[nodiscard]] std::size_t requestBodySize(const HttpRequest& req) noexcept {
    if (!req.multipart.empty()) {
        std::size_t total = 0;
        for (const auto& part : req.multipart) {
            total += part.value.size();
        }
        return total;
    }
    return req.body ? req.body->size() : 0U;
}

// Opt-in auth-flow response body (nullopt unless capture is on), capped UTF-8-safely; auth bodies
// carry tokens.
[[nodiscard]] std::optional<std::string> capturedBody(const std::string& body, bool capture) {
    if (!capture) {
        return std::nullopt;
    }
    if (body.size() > kMaxCapturedBodyBytes) {
        return truncateUtf8(body, kMaxCapturedBodyBytes);
    }
    return body;
}

class ChainAuthenticator final : public Authenticator {
public:
    explicit ChainAuthenticator(AuthDependencies deps) : deps_(std::move(deps)) {}

    std::expected<ActorSession, ReqloomError> authenticate(const Actor& actor,
                                                           RunContext& ctx,
                                                           const ResolveContext& rctx) override {
        // nullptr is an engine programming error, not user-triggerable.
        if ((deps_.http == nullptr) || (deps_.varResolver == nullptr)) {
            return std::unexpected(ReqloomError{ErrorCode::SessionRefreshFailed,
                                                ErrorClass::Auth,
                                                "auth: authenticator wired without dependencies"});
        }

        ActorSession session;
        session.state = ActorSession::State::Authenticating;

        for (const auto& step : actor.authSteps) {
            auto resolvedPath = deps_.varResolver->resolveUrlPath(step.pathTemplate, ctx, rctx);
            if (!resolvedPath.unresolved.empty()) {
                return std::unexpected(ReqloomError{
                    ErrorCode::SessionRefreshFailed,
                    ErrorClass::Auth,
                    "auth: unresolved variable in path: " + resolvedPath.unresolved.front()});
            }

            HttpRequest req;
            req.method = step.method;
            req.transport = rctx.transport;
            const auto baseUrlIt = rctx.envVars.find("baseUrl");
            const std::string baseUrl = baseUrlIt != rctx.envVars.end() ? baseUrlIt->second : "";
            req.url = baseUrl + resolvedPath.output;

            for (const auto& [k, v] : step.headers) {
                auto resolved = deps_.varResolver->resolve(v, ctx, rctx);
                req.headers[k] = resolved.output;
            }

            if (step.bodyTemplate) {
                auto resolved = resolveRequestBody(
                    *deps_.varResolver, *step.bodyTemplate, req.headers, ctx, rctx);
                req.body = resolved.output;
                if (!req.headers.contains("Content-Type")) {
                    req.headers["Content-Type"] = "application/json";
                }
            }

            if (deps_.agentOutput) {
                registerSentCredentials(req, ctx.secretRegistry());
            }
            if (deps_.emit) {
                // runId/stepIndex are the parent step's so the timeline groups the auth request
                // under its op.
                deps_.emit(RequestPrepared{deps_.runId,
                                           deps_.stepIndex,
                                           req.method,
                                           req.url,
                                           snapshotMaskedRequestHeaders(req),
                                           requestBodySize(req),
                                           std::chrono::system_clock::now(),
                                           deps_.exchange});
            }

            req.cancel = deps_.cancel;
            auto response = deps_.http->send(req);
            if (!response && response.error().code == ErrorCode::HostNotApproved) {
                return std::unexpected(response.error());
            }
            if (!response) {
                return std::unexpected(ReqloomError{ErrorCode::SessionRefreshFailed,
                                                    ErrorClass::Auth,
                                                    "auth: network error during step '" + step.id +
                                                        "': " + response.error().detail});
            }

            if (deps_.emit) {
                deps_.emit(
                    ResponseReceived{deps_.runId,
                                     deps_.stepIndex,
                                     response->status,
                                     maskHeaders(response->headers),
                                     response->body.size(),
                                     response->elapsed,
                                     std::chrono::system_clock::now(),
                                     capturedBody(response->body, deps_.captureResponseBodies),
                                     deps_.exchange});
            }

            // Absorb Set-Cookie so later operations as this actor carry session cookies (PHP/Rails
            // /login).
            cookies::absorbSetCookies(response->headers, actor.id, ctx, req.url);

            if (step.expectStatus && response->status != *step.expectStatus) {
                const auto excerpt =
                    safeBodyExcerpt(response->body,
                                    &ctx.secretRegistry(),
                                    deps_.failureExcerptBytes,
                                    deps_.agentOutput ? MaskProfile::Agent : MaskProfile::Display);
                return std::unexpected(
                    ReqloomError{ErrorCode::SessionRefreshFailed,
                                 ErrorClass::Auth,
                                 "auth: step '" + step.id + "' returned HTTP " +
                                     std::to_string(response->status) + " (expected " +
                                     std::to_string(*step.expectStatus) + ")" +
                                     (excerpt ? " — " + *excerpt : std::string{})});
            }

            if (!response->body.empty() && !step.extractions.empty()) {
                auto values = extractFromJson(response->body, step.extractions);
                if (!values) {
                    return std::unexpected(ReqloomError{ErrorCode::SessionRefreshFailed,
                                                        ErrorClass::Auth,
                                                        "auth: extraction failed in step '" +
                                                            step.id +
                                                            "': " + values.error().detail});
                }
                for (auto& [k, v] : *values) {
                    // Register now, not at putSession, so later auth steps' events are redacted.
                    ctx.secretRegistry().add(v);
                    session.variables[k] = std::move(v);
                }
            }
        }

        return session;
    }

private:
    AuthDependencies deps_;
};

// AuthStrategy::Basic (RFC 7617); no HTTP call.
class BasicAuthenticator final : public Authenticator {
public:
    explicit BasicAuthenticator(AuthDependencies deps) : deps_(std::move(deps)) {}

    std::expected<ActorSession, ReqloomError> authenticate(const Actor& actor,
                                                           RunContext& ctx,
                                                           const ResolveContext& rctx) override {
        if (deps_.varResolver == nullptr) {
            return std::unexpected(
                ReqloomError{ErrorCode::SessionRefreshFailed,
                             ErrorClass::Auth,
                             "auth: basic authenticator wired without resolver"});
        }

        const auto userIt = actor.authConfig.find("username");
        const auto passIt = actor.authConfig.find("password");
        if (userIt == actor.authConfig.end() || passIt == actor.authConfig.end()) {
            return std::unexpected(
                ReqloomError{ErrorCode::SessionRefreshFailed,
                             ErrorClass::Auth,
                             "auth: basic strategy requires `username` and `password` "
                             "under actor.auth"});
        }

        auto userResolved = deps_.varResolver->resolve(userIt->second, ctx, rctx);
        if (!userResolved.unresolved.empty()) {
            return std::unexpected(ReqloomError{ErrorCode::SessionRefreshFailed,
                                                ErrorClass::Auth,
                                                "auth: basic.username has unresolved variable: " +
                                                    userResolved.unresolved.front()});
        }
        auto passResolved = deps_.varResolver->resolve(passIt->second, ctx, rctx);
        if (!passResolved.unresolved.empty()) {
            return std::unexpected(ReqloomError{ErrorCode::SessionRefreshFailed,
                                                ErrorClass::Auth,
                                                "auth: basic.password has unresolved variable: " +
                                                    passResolved.unresolved.front()});
        }

        ctx.secretRegistry().add(passResolved.output);
        // user:pass registers its base64 spelling too, i.e. the credential.
        ctx.secretRegistry().add(userResolved.output + ":" + passResolved.output);
        ActorSession session;
        session.state = ActorSession::State::Authenticating;
        session.variables["credential"] =
            base64Encode(userResolved.output + ":" + passResolved.output);
        return session;
    }

private:
    AuthDependencies deps_;
};

/// AuthStrategy::ApiKey; no HTTP call. Reads `key`, `location` (header|query) and `name` from
/// `actor.authConfig`; injects automatically only when both location and name are set.
/// The cookie location is rejected.
class ApiKeyAuthenticator final : public Authenticator {
public:
    explicit ApiKeyAuthenticator(AuthDependencies deps) : deps_(std::move(deps)) {}

    std::expected<ActorSession, ReqloomError> authenticate(const Actor& actor,
                                                           RunContext& ctx,
                                                           const ResolveContext& rctx) override {
        if (deps_.varResolver == nullptr) {
            return std::unexpected(
                ReqloomError{ErrorCode::SessionRefreshFailed,
                             ErrorClass::Auth,
                             "auth: api_key authenticator wired without resolver"});
        }

        const auto keyIt = actor.authConfig.find("key");
        if (keyIt == actor.authConfig.end()) {
            return std::unexpected(
                ReqloomError{ErrorCode::SessionRefreshFailed,
                             ErrorClass::Auth,
                             "auth: api_key strategy requires `key` under actor.auth"});
        }
        const auto resolvedKey = deps_.varResolver->resolve(keyIt->second, ctx, rctx);
        if (!resolvedKey.unresolved.empty()) {
            return std::unexpected(ReqloomError{
                ErrorCode::SessionRefreshFailed,
                ErrorClass::Auth,
                "auth: api_key.key has unresolved variable: " + resolvedKey.unresolved.front()});
        }

        ActorSession session;
        session.state = ActorSession::State::Authenticating;
        session.variables["key"] = resolvedKey.output;

        const auto locIt = actor.authConfig.find("location");
        const auto nameIt = actor.authConfig.find("name");
        if (locIt != actor.authConfig.end() && nameIt != actor.authConfig.end()) {
            const auto& location = locIt->second;
            if (location == "header") {
                session.injectHeaders[nameIt->second] = resolvedKey.output;
            } else if (location == "query") {
                session.injectQueryParams[nameIt->second] = resolvedKey.output;
            } else if (location == "cookie") {
                return std::unexpected(
                    ReqloomError{ErrorCode::SessionRefreshFailed,
                                 ErrorClass::Auth,
                                 "auth: api_key location 'cookie' is not yet supported "
                                 "(use a manual inject: header with Cookie: ...)"});
            } else {
                return std::unexpected(
                    ReqloomError{ErrorCode::SessionRefreshFailed,
                                 ErrorClass::Auth,
                                 "auth: api_key.location must be 'header', 'query', "
                                 "or 'cookie'; got: " +
                                     location});
            }
        }

        return session;
    }

private:
    AuthDependencies deps_;
};

/// Shared OAuth2 token-endpoint flow; `formBody` is the urlencoded payload.
std::expected<ActorSession, ReqloomError> executeOAuth2TokenRequest(
    HttpClient& http,
    const std::string& tokenUrl,
    std::string formBody,
    std::string_view strategyLabel,
    const TransportConfig& transport,
    const AuthDependencies& deps,
    const std::optional<std::string>& authorizationHeader = std::nullopt) {
    HttpRequest req;
    req.method = HttpMethod::Post;
    req.url = tokenUrl;
    req.headers["Content-Type"] = "application/x-www-form-urlencoded";
    req.headers["Accept"] = "application/json";
    // RFC 6749 §2.3.1: credentials go in the body or an HTTP Basic header; with the header the body
    // omits them.
    if (authorizationHeader) {
        req.headers["Authorization"] = *authorizationHeader;
    }
    req.body = std::move(formBody);
    req.transport = transport;

    if (deps.emit) {
        deps.emit(RequestPrepared{deps.runId,
                                  deps.stepIndex,
                                  req.method,
                                  req.url,
                                  snapshotMaskedRequestHeaders(req),
                                  requestBodySize(req),
                                  std::chrono::system_clock::now(),
                                  deps.exchange});
    }

    req.cancel = deps.cancel;
    const auto response = http.send(req);
    if (!response && response.error().code == ErrorCode::HostNotApproved) {
        return std::unexpected(response.error());
    }
    if (!response) {
        return std::unexpected(ReqloomError{
            ErrorCode::SessionRefreshFailed,
            ErrorClass::Auth,
            "auth: " + std::string{strategyLabel} + " network error: " + response.error().detail});
    }

    // Surface the token response (failures too) for the timeline; body only when capture is on.
    if (deps.emit) {
        deps.emit(ResponseReceived{deps.runId,
                                   deps.stepIndex,
                                   response->status,
                                   maskHeaders(response->headers),
                                   response->body.size(),
                                   response->elapsed,
                                   std::chrono::system_clock::now(),
                                   capturedBody(response->body, deps.captureResponseBodies),
                                   deps.exchange});
    }

    if (response->status < 200 || response->status >= 300) {
        // Sensitive fields are masked; a failed token response may still carry a token.
        const auto excerpt =
            safeBodyExcerpt(response->body,
                            nullptr,
                            deps.failureExcerptBytes,
                            deps.agentOutput ? MaskProfile::Agent : MaskProfile::Display);
        return std::unexpected(ReqloomError{
            ErrorCode::SessionRefreshFailed,
            ErrorClass::Auth,
            "auth: " + std::string{strategyLabel} + " token endpoint returned HTTP " +
                std::to_string(response->status) + (excerpt ? " — " + *excerpt : std::string{})});
    }

    // Parse once and read every field from the one document.
    json doc;
    try {
        doc = json::parse(response->body);
    } catch (const json::parse_error& e) {
        // e.what() quotes the input near the error, which may be the token itself.
        return std::unexpected(ReqloomError{ErrorCode::SessionRefreshFailed,
                                            ErrorClass::Auth,
                                            "auth: " + std::string{strategyLabel} +
                                                " response is not valid JSON (syntax error at "
                                                "byte " +
                                                std::to_string(e.byte) + ")"});
    }

    const auto accessTokenIt = doc.find("access_token");
    if (accessTokenIt == doc.end() || !accessTokenIt->is_string()) {
        return std::unexpected(ReqloomError{
            ErrorCode::SessionRefreshFailed,
            ErrorClass::Auth,
            "auth: " + std::string{strategyLabel} + " response missing string `access_token`"});
    }

    ActorSession session;
    session.state = ActorSession::State::Authenticating;
    auto accessToken = accessTokenIt->get<std::string>();

    // Captured one at a time so a missing `expires_in` doesn't suppress `token_type`.
    for (const auto* optionalField : {"expires_in", "token_type", "scope", "refresh_token"}) {
        const auto it = doc.find(optionalField);
        if (it == doc.end() || it->is_null()) {
            continue;
        }
        session.variables[optionalField] = it->is_string() ? it->get<std::string>() : it->dump();
    }

    session.injectHeaders["Authorization"] = "Bearer " + accessToken;
    session.variables["access_token"] = std::move(accessToken);
    return session;
}

/// Resolve `authConfig[fieldName]`; empty strings count as missing.
std::expected<std::string, ReqloomError> resolveAuthConfigField(const Actor& actor,
                                                                const RunContext& ctx,
                                                                const ResolveContext& rctx,
                                                                VariableResolver& resolver,
                                                                std::string_view strategyLabel,
                                                                std::string_view fieldName) {
    const auto it = actor.authConfig.find(std::string{fieldName});
    if (it == actor.authConfig.end() || it->second.empty()) {
        return std::unexpected(ReqloomError{ErrorCode::SessionRefreshFailed,
                                            ErrorClass::Auth,
                                            "auth: " + std::string{strategyLabel} + " requires `" +
                                                std::string{fieldName} + "` under actor.auth"});
    }
    auto resolved = resolver.resolve(it->second, ctx, rctx);
    if (!resolved.unresolved.empty()) {
        return std::unexpected(
            ReqloomError{ErrorCode::SessionRefreshFailed,
                         ErrorClass::Auth,
                         "auth: " + std::string{strategyLabel} + "." + std::string{fieldName} +
                             " has unresolved variable: " + resolved.unresolved.front()});
    }
    return resolved.output;
}

std::expected<std::string, ReqloomError> resolveAuthConfigOptional(const Actor& actor,
                                                                   const RunContext& ctx,
                                                                   const ResolveContext& rctx,
                                                                   VariableResolver& resolver,
                                                                   std::string_view strategyLabel,
                                                                   std::string_view fieldName) {
    const auto it = actor.authConfig.find(std::string{fieldName});
    if (it == actor.authConfig.end() || it->second.empty()) {
        return std::string{};
    }
    auto resolved = resolver.resolve(it->second, ctx, rctx);
    if (!resolved.unresolved.empty()) {
        return std::unexpected(
            ReqloomError{ErrorCode::SessionRefreshFailed,
                         ErrorClass::Auth,
                         "auth: " + std::string{strategyLabel} + "." + std::string{fieldName} +
                             " has unresolved variable: " + resolved.unresolved.front()});
    }
    return resolved.output;
}

// AuthStrategy::OAuth2ClientCredentials (RFC 6749 §4.4).
class OAuth2ClientCredentialsAuthenticator final : public Authenticator {
public:
    explicit OAuth2ClientCredentialsAuthenticator(AuthDependencies deps) : deps_(std::move(deps)) {}

    std::expected<ActorSession, ReqloomError> authenticate(const Actor& actor,
                                                           RunContext& ctx,
                                                           const ResolveContext& rctx) override {
        if ((deps_.http == nullptr) || (deps_.varResolver == nullptr)) {
            return std::unexpected(
                ReqloomError{ErrorCode::SessionRefreshFailed,
                             ErrorClass::Auth,
                             "auth: oauth2_client_credentials authenticator wired "
                             "without dependencies"});
        }
        constexpr std::string_view kLabel = "oauth2_client_credentials";

        const auto tokenUrl =
            resolveAuthConfigField(actor, ctx, rctx, *deps_.varResolver, kLabel, "token_url");
        if (!tokenUrl) {
            return std::unexpected(tokenUrl.error());
        }
        const auto clientId =
            resolveAuthConfigField(actor, ctx, rctx, *deps_.varResolver, kLabel, "client_id");
        if (!clientId) {
            return std::unexpected(clientId.error());
        }
        const auto scope =
            resolveAuthConfigOptional(actor, ctx, rctx, *deps_.varResolver, kLabel, "scope");
        if (!scope) {
            return std::unexpected(scope.error());
        }

        const auto clientAuth =
            resolveAuthConfigOptional(actor, ctx, rctx, *deps_.varResolver, kLabel, "client_auth");
        if (!clientAuth) {
            return std::unexpected(clientAuth.error());
        }

        // client_secret is required unless client_auth is none (public client), RFC 6749 §2.3.1.
        std::string clientSecret;
        if (*clientAuth != "none") {
            auto cs = resolveAuthConfigField(
                actor, ctx, rctx, *deps_.varResolver, kLabel, "client_secret");
            if (!cs) {
                return std::unexpected(cs.error());
            }
            clientSecret = *cs;
        }
        ctx.secretRegistry().add(clientSecret);

        // RFC 6749 §4.4.2; `client_auth` picks Basic header, body or none.
        std::string body = "grant_type=client_credentials";
        std::optional<std::string> authHeader;
        if (*clientAuth == "basic") {
            authHeader = "Basic " + base64Encode(*clientId + ":" + clientSecret);
        } else if (*clientAuth == "none") {
            body += "&client_id=" + urlEncode(*clientId);
        } else {
            body +=
                "&client_id=" + urlEncode(*clientId) + "&client_secret=" + urlEncode(clientSecret);
        }
        if (!scope->empty()) {
            body += "&scope=" + urlEncode(*scope);
        }

        return executeOAuth2TokenRequest(
            *deps_.http, *tokenUrl, std::move(body), kLabel, rctx.transport, deps_, authHeader);
    }

private:
    AuthDependencies deps_;
};

// AuthStrategy::OAuth2Password (RFC 6749 §4.3).
class OAuth2PasswordAuthenticator final : public Authenticator {
public:
    explicit OAuth2PasswordAuthenticator(AuthDependencies deps) : deps_(std::move(deps)) {}

    std::expected<ActorSession, ReqloomError> authenticate(const Actor& actor,
                                                           RunContext& ctx,
                                                           const ResolveContext& rctx) override {
        if ((deps_.http == nullptr) || (deps_.varResolver == nullptr)) {
            return std::unexpected(ReqloomError{ErrorCode::SessionRefreshFailed,
                                                ErrorClass::Auth,
                                                "auth: oauth2_password authenticator wired without "
                                                "dependencies"});
        }
        constexpr std::string_view kLabel = "oauth2_password";

        const auto tokenUrl =
            resolveAuthConfigField(actor, ctx, rctx, *deps_.varResolver, kLabel, "token_url");
        if (!tokenUrl) {
            return std::unexpected(tokenUrl.error());
        }
        const auto clientId =
            resolveAuthConfigField(actor, ctx, rctx, *deps_.varResolver, kLabel, "client_id");
        if (!clientId) {
            return std::unexpected(clientId.error());
        }
        const auto username =
            resolveAuthConfigField(actor, ctx, rctx, *deps_.varResolver, kLabel, "username");
        if (!username) {
            return std::unexpected(username.error());
        }
        const auto password =
            resolveAuthConfigField(actor, ctx, rctx, *deps_.varResolver, kLabel, "password");
        if (!password) {
            return std::unexpected(password.error());
        }
        ctx.secretRegistry().add(*password);
        const auto scope =
            resolveAuthConfigOptional(actor, ctx, rctx, *deps_.varResolver, kLabel, "scope");
        if (!scope) {
            return std::unexpected(scope.error());
        }

        const auto clientAuth =
            resolveAuthConfigOptional(actor, ctx, rctx, *deps_.varResolver, kLabel, "client_auth");
        if (!clientAuth) {
            return std::unexpected(clientAuth.error());
        }

        // client_secret is required unless client_auth is none (public client), RFC 6749 §2.3.1.
        std::string clientSecret;
        if (*clientAuth != "none") {
            auto cs = resolveAuthConfigField(
                actor, ctx, rctx, *deps_.varResolver, kLabel, "client_secret");
            if (!cs) {
                return std::unexpected(cs.error());
            }
            clientSecret = *cs;
        }
        ctx.secretRegistry().add(clientSecret);

        // RFC 6749 §4.3.2; `client_auth` picks Basic header, body or none.
        std::string body = "grant_type=password&username=" + urlEncode(*username) +
                           "&password=" + urlEncode(*password);
        std::optional<std::string> authHeader;
        if (*clientAuth == "basic") {
            authHeader = "Basic " + base64Encode(*clientId + ":" + clientSecret);
        } else if (*clientAuth == "none") {
            body += "&client_id=" + urlEncode(*clientId);
        } else {
            body +=
                "&client_id=" + urlEncode(*clientId) + "&client_secret=" + urlEncode(clientSecret);
        }
        if (!scope->empty()) {
            body += "&scope=" + urlEncode(*scope);
        }

        return executeOAuth2TokenRequest(
            *deps_.http, *tokenUrl, std::move(body), kLabel, rctx.transport, deps_, authHeader);
    }

private:
    AuthDependencies deps_;
};

// AuthStrategy::OAuth1 (RFC 5849, two-legged HMAC-SHA1); the executor signs each request.
class OAuth1Authenticator final : public Authenticator {
public:
    explicit OAuth1Authenticator(AuthDependencies deps) : deps_(std::move(deps)) {}

    std::expected<ActorSession, ReqloomError> authenticate(const Actor& actor,
                                                           RunContext& ctx,
                                                           const ResolveContext& rctx) override {
        if (deps_.varResolver == nullptr) {
            return std::unexpected(
                ReqloomError{ErrorCode::SessionRefreshFailed,
                             ErrorClass::Auth,
                             "auth: oauth1 authenticator wired without resolver"});
        }
        constexpr std::string_view kLabel = "oauth1";

        const auto consumerKey =
            resolveAuthConfigField(actor, ctx, rctx, *deps_.varResolver, kLabel, "consumer_key");
        if (!consumerKey) {
            return std::unexpected(consumerKey.error());
        }
        const auto consumerSecret =
            resolveAuthConfigField(actor, ctx, rctx, *deps_.varResolver, kLabel, "consumer_secret");
        if (!consumerSecret) {
            return std::unexpected(consumerSecret.error());
        }

        const auto token =
            resolveAuthConfigOptional(actor, ctx, rctx, *deps_.varResolver, kLabel, "token");
        if (!token) {
            return std::unexpected(token.error());
        }
        const auto tokenSecret =
            resolveAuthConfigOptional(actor, ctx, rctx, *deps_.varResolver, kLabel, "token_secret");
        if (!tokenSecret) {
            return std::unexpected(tokenSecret.error());
        }
        const auto realm =
            resolveAuthConfigOptional(actor, ctx, rctx, *deps_.varResolver, kLabel, "realm");
        if (!realm) {
            return std::unexpected(realm.error());
        }

        // Token + token_secret must come as a pair (RFC 5849 §3.1).
        if (token->empty() != tokenSecret->empty()) {
            return std::unexpected(
                ReqloomError{ErrorCode::SessionRefreshFailed,
                             ErrorClass::Auth,
                             "auth: oauth1 requires `token` and `token_secret` to be "
                             "set together (or both omitted for two-legged signing)"});
        }

        ActorSession session;
        session.state = ActorSession::State::Authenticating;
        session.variables["consumer_key"] = *consumerKey;
        session.variables["consumer_secret"] = *consumerSecret;
        if (!token->empty()) {
            session.variables["token"] = *token;
        }
        if (!tokenSecret->empty()) {
            session.variables["token_secret"] = *tokenSecret;
        }
        if (!realm->empty()) {
            session.variables["realm"] = *realm;
        }
        session.signingScheme = ActorSession::SigningScheme::OAuth1HmacSha1;
        return session;
    }

private:
    AuthDependencies deps_;
};

// AuthStrategy::AwsSigV4; the executor signs each request.
class AwsSigV4Authenticator final : public Authenticator {
public:
    explicit AwsSigV4Authenticator(AuthDependencies deps) : deps_(std::move(deps)) {}

    std::expected<ActorSession, ReqloomError> authenticate(const Actor& actor,
                                                           RunContext& ctx,
                                                           const ResolveContext& rctx) override {
        if (deps_.varResolver == nullptr) {
            return std::unexpected(
                ReqloomError{ErrorCode::SessionRefreshFailed,
                             ErrorClass::Auth,
                             "auth: aws_sigv4 authenticator wired without resolver"});
        }
        constexpr std::string_view kLabel = "aws_sigv4";

        const auto accessKey =
            resolveAuthConfigField(actor, ctx, rctx, *deps_.varResolver, kLabel, "access_key");
        if (!accessKey) {
            return std::unexpected(accessKey.error());
        }
        const auto secretKey =
            resolveAuthConfigField(actor, ctx, rctx, *deps_.varResolver, kLabel, "secret_key");
        if (!secretKey) {
            return std::unexpected(secretKey.error());
        }
        const auto region =
            resolveAuthConfigField(actor, ctx, rctx, *deps_.varResolver, kLabel, "region");
        if (!region) {
            return std::unexpected(region.error());
        }
        const auto service =
            resolveAuthConfigField(actor, ctx, rctx, *deps_.varResolver, kLabel, "service");
        if (!service) {
            return std::unexpected(service.error());
        }

        const auto sessionToken = resolveAuthConfigOptional(
            actor, ctx, rctx, *deps_.varResolver, kLabel, "session_token");
        if (!sessionToken) {
            return std::unexpected(sessionToken.error());
        }

        ActorSession session;
        session.state = ActorSession::State::Authenticating;
        session.variables["access_key"] = *accessKey;
        session.variables["secret_key"] = *secretKey;
        session.variables["region"] = *region;
        session.variables["service"] = *service;
        if (!sessionToken->empty()) {
            session.variables["session_token"] = *sessionToken;
        }
        session.signingScheme = ActorSession::SigningScheme::AwsSigV4;
        return session;
    }

private:
    AuthDependencies deps_;
};

// AuthStrategy::Bearer; no HTTP call.
class BearerAuthenticator final : public Authenticator {
public:
    explicit BearerAuthenticator(AuthDependencies deps) : deps_(std::move(deps)) {}

    std::expected<ActorSession, ReqloomError> authenticate(const Actor& actor,
                                                           RunContext& ctx,
                                                           const ResolveContext& rctx) override {
        if (deps_.varResolver == nullptr) {
            return std::unexpected(ReqloomError{ErrorCode::SessionRefreshFailed,
                                                ErrorClass::Auth,
                                                "auth: bearer authenticator wired without "
                                                "resolver"});
        }
        const auto token =
            resolveAuthConfigField(actor, ctx, rctx, *deps_.varResolver, "bearer", "token");
        if (!token) {
            return std::unexpected(token.error());
        }
        ActorSession session;
        session.state = ActorSession::State::Authenticating;
        session.variables["token"] = *token;
        session.injectHeaders["Authorization"] = "Bearer " + *token;
        return session;
    }

private:
    AuthDependencies deps_;
};

// AuthStrategy::Jwt: HS256/HS512 only. Signed once at session creation, so time-based claims
// (exp/iat) are fixed for the session lifetime.
class JwtAuthenticator final : public Authenticator {
public:
    explicit JwtAuthenticator(AuthDependencies deps) : deps_(std::move(deps)) {}

    std::expected<ActorSession, ReqloomError> authenticate(const Actor& actor,
                                                           RunContext& ctx,
                                                           const ResolveContext& rctx) override {
        if (deps_.varResolver == nullptr) {
            return std::unexpected(ReqloomError{ErrorCode::SessionRefreshFailed,
                                                ErrorClass::Auth,
                                                "auth: jwt authenticator wired without resolver"});
        }
        const auto secret =
            resolveAuthConfigField(actor, ctx, rctx, *deps_.varResolver, "jwt", "secret");
        if (!secret) {
            return std::unexpected(secret.error());
        }
        ctx.secretRegistry().add(*secret);
        const auto payload =
            resolveAuthConfigOptional(actor, ctx, rctx, *deps_.varResolver, "jwt", "payload");
        if (!payload) {
            return std::unexpected(payload.error());
        }

        std::string algo = actor.authConfig.contains("algorithm") ? actor.authConfig.at("algorithm")
                                                                  : std::string{};
        std::ranges::transform(
            algo, algo.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (algo.empty()) {
            algo = "HS256";
        }
        if (algo != "HS256" && algo != "HS512") {
            return std::unexpected(
                ReqloomError{ErrorCode::SessionRefreshFailed,
                             ErrorClass::Auth,
                             "auth: jwt.algorithm '" + algo +
                                 "' unsupported (only HS256 and HS512 are supported)"});
        }
        const std::string claims = payload->empty() ? std::string{"{}"} : *payload;
        const std::string jwt = algo == "HS512" ? crypto::jwtSignHs512(claims, *secret)
                                                : crypto::jwtSignHs256(claims, *secret);
        if (jwt.empty()) {
            return std::unexpected(ReqloomError{ErrorCode::SessionRefreshFailed,
                                                ErrorClass::Auth,
                                                "auth: jwt signing failed (empty secret or "
                                                "crypto error)"});
        }
        ActorSession session;
        session.state = ActorSession::State::Authenticating;
        session.injectHeaders["Authorization"] = "Bearer " + jwt;
        return session;
    }

private:
    AuthDependencies deps_;
};

// AuthStrategy::Mtls: client cert/key paths go on the session TransportConfig. Paths are not
// sandboxed to the project root (client certs commonly live outside it).
class MtlsAuthenticator final : public Authenticator {
public:
    explicit MtlsAuthenticator(AuthDependencies deps) : deps_(std::move(deps)) {}

    std::expected<ActorSession, ReqloomError> authenticate(const Actor& actor,
                                                           RunContext& ctx,
                                                           const ResolveContext& rctx) override {
        if (deps_.varResolver == nullptr) {
            return std::unexpected(ReqloomError{ErrorCode::SessionRefreshFailed,
                                                ErrorClass::Auth,
                                                "auth: mtls authenticator wired without resolver"});
        }
        constexpr std::string_view kLabel = "mtls";
        const auto certPath =
            resolveAuthConfigField(actor, ctx, rctx, *deps_.varResolver, kLabel, "cert_path");
        if (!certPath) {
            return std::unexpected(certPath.error());
        }
        const auto keyPath =
            resolveAuthConfigOptional(actor, ctx, rctx, *deps_.varResolver, kLabel, "key_path");
        if (!keyPath) {
            return std::unexpected(keyPath.error());
        }
        const auto keyPassword =
            resolveAuthConfigOptional(actor, ctx, rctx, *deps_.varResolver, kLabel, "key_password");
        if (!keyPassword) {
            return std::unexpected(keyPassword.error());
        }
        const auto caCertPath =
            resolveAuthConfigOptional(actor, ctx, rctx, *deps_.varResolver, kLabel, "ca_cert_path");
        if (!caCertPath) {
            return std::unexpected(caCertPath.error());
        }

        const bool p12 = [&]() {
            std::string format =
                actor.authConfig.contains("format") ? actor.authConfig.at("format") : std::string{};
            std::ranges::transform(format, format.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return format == "p12";
        }();

        TransportConfig transport;
        transport.clientCertPath = *certPath;
        if (p12) {
            // PKCS#12 bundle: the key lives inside the cert; tell curl the type.
            transport.clientCertType = "P12";
        } else if (!keyPath->empty()) {
            transport.clientKeyPath = *keyPath;
        }
        if (!keyPassword->empty()) {
            transport.clientKeyPassword = *keyPassword;
        }
        if (!caCertPath->empty()) {
            transport.caBundlePath = *caCertPath;
        }

        ActorSession session;
        session.state = ActorSession::State::Authenticating;
        session.transport = std::move(transport);
        return session;
    }

private:
    AuthDependencies deps_;
};

}  // namespace

std::unique_ptr<Authenticator> selectAuthenticator(const Actor& actor, AuthDependencies deps) {
    switch (actor.strategy) {
        case AuthStrategy::Simple:
        case AuthStrategy::Chain:
            return std::make_unique<ChainAuthenticator>(std::move(deps));
        case AuthStrategy::Basic:
            return std::make_unique<BasicAuthenticator>(std::move(deps));
        case AuthStrategy::ApiKey:
            return std::make_unique<ApiKeyAuthenticator>(std::move(deps));
        case AuthStrategy::OAuth2ClientCredentials:
            return std::make_unique<OAuth2ClientCredentialsAuthenticator>(std::move(deps));
        case AuthStrategy::OAuth2Password:
            return std::make_unique<OAuth2PasswordAuthenticator>(std::move(deps));
        case AuthStrategy::OAuth1:
            return std::make_unique<OAuth1Authenticator>(std::move(deps));
        case AuthStrategy::AwsSigV4:
            return std::make_unique<AwsSigV4Authenticator>(std::move(deps));
        case AuthStrategy::Bearer:
            return std::make_unique<BearerAuthenticator>(std::move(deps));
        case AuthStrategy::Jwt:
            return std::make_unique<JwtAuthenticator>(std::move(deps));
        case AuthStrategy::Mtls:
            return std::make_unique<MtlsAuthenticator>(std::move(deps));
    }
    return nullptr;
}

std::chrono::seconds sessionLifetime(const Actor& actor,
                                     const std::map<std::string, std::string>& variables) {
    const bool isOAuth2 = actor.strategy == AuthStrategy::OAuth2ClientCredentials ||
                          actor.strategy == AuthStrategy::OAuth2Password;
    const auto it = variables.find("expires_in");
    if (!isOAuth2 || it == variables.end()) {
        return actor.sessionTtl;
    }
    std::int64_t secs{0};
    const auto& text = it->second;
    if (std::from_chars(text.data(), text.data() + text.size(), secs).ec != std::errc{} ||
        secs <= 0) {
        return actor.sessionTtl;
    }
    const std::chrono::seconds lifetime{secs};
    const auto usable = lifetime > kTokenExpiryMargin ? lifetime - kTokenExpiryMargin : lifetime;
    return std::min(usable, actor.sessionTtl);
}

std::expected<std::map<std::string, std::string>, ReqloomError> runRefresh(
    const Actor& actor, RunContext& ctx, const ResolveContext& rctx, const AuthDependencies& deps) {
    if (!actor.refresh) {
        return std::unexpected(ReqloomError{
            ErrorCode::SessionRefreshFailed,
            ErrorClass::Auth,
            "refresh: actor '" + actor.id.value + "' has no `session.refresh:` block defined"});
    }
    if ((deps.http == nullptr) || (deps.varResolver == nullptr)) {
        return std::unexpected(ReqloomError{
            ErrorCode::SessionRefreshFailed, ErrorClass::Auth, "refresh: dependencies not wired"});
    }

    const auto& refresh = *actor.refresh;
    auto resolvedPath = deps.varResolver->resolveUrlPath(refresh.pathTemplate, ctx, rctx);
    if (!resolvedPath.unresolved.empty()) {
        return std::unexpected(ReqloomError{
            ErrorCode::SessionRefreshFailed,
            ErrorClass::Auth,
            "refresh: unresolved variable in path: " + resolvedPath.unresolved.front()});
    }

    HttpRequest req;
    req.method = refresh.method;
    req.transport = rctx.transport;
    const auto baseUrlIt = rctx.envVars.find("baseUrl");
    const std::string baseUrl = baseUrlIt != rctx.envVars.end() ? baseUrlIt->second : "";
    req.url = baseUrl + resolvedPath.output;

    for (const auto& [k, v] : refresh.headers) {
        auto resolved = deps.varResolver->resolve(v, ctx, rctx);
        req.headers[k] = resolved.output;
    }
    if (refresh.bodyTemplate) {
        auto resolved =
            resolveRequestBody(*deps.varResolver, *refresh.bodyTemplate, req.headers, ctx, rctx);
        req.body = resolved.output;
        if (!req.headers.contains("Content-Type")) {
            req.headers["Content-Type"] = "application/json";
        }
    }

    if (deps.agentOutput) {
        registerSentCredentials(req, ctx.secretRegistry());
    }
    if (deps.emit) {
        deps.emit(RequestPrepared{deps.runId,
                                  deps.stepIndex,
                                  req.method,
                                  req.url,
                                  snapshotMaskedRequestHeaders(req),
                                  requestBodySize(req),
                                  std::chrono::system_clock::now(),
                                  ExchangeKind::SessionRefresh});
    }

    req.cancel = deps.cancel;
    auto response = deps.http->send(req);
    if (!response && response.error().code == ErrorCode::HostNotApproved) {
        return std::unexpected(response.error());
    }
    if (!response) {
        return std::unexpected(ReqloomError{ErrorCode::SessionRefreshFailed,
                                            ErrorClass::Auth,
                                            "refresh: network error: " + response.error().detail});
    }

    if (deps.emit) {
        deps.emit(ResponseReceived{deps.runId,
                                   deps.stepIndex,
                                   response->status,
                                   maskHeaders(response->headers),
                                   response->body.size(),
                                   response->elapsed,
                                   std::chrono::system_clock::now(),
                                   capturedBody(response->body, deps.captureResponseBodies),
                                   ExchangeKind::SessionRefresh});
    }

    // Absorb Set-Cookie from the refresh response; endpoints commonly rotate session cookies.
    cookies::absorbSetCookies(response->headers, actor.id, ctx, req.url);

    // `expect_status:` when set, otherwise any 2xx.
    const auto statusOk = [&]() {
        if (!refresh.expectStatusList.empty()) {
            return std::find(refresh.expectStatusList.begin(),
                             refresh.expectStatusList.end(),
                             response->status) != refresh.expectStatusList.end();
        }
        if (refresh.expectStatus) {
            return response->status == *refresh.expectStatus;
        }
        return response->status >= 200 && response->status < 300;
    }();

    if (!statusOk) {
        return std::unexpected(ReqloomError{
            ErrorCode::SessionRefreshFailed,
            ErrorClass::Auth,
            "refresh: HTTP " + std::to_string(response->status) +
                " (refresh endpoint rejected the existing credentials — caller should re-auth)"});
    }

    if (refresh.extractions.empty()) {
        // No extractions: return an empty map so the caller can update expiresAt without touching
        // session vars.
        return std::map<std::string, std::string>{};
    }
    auto values = extractFromJson(response->body, refresh.extractions);
    if (!values) {
        return std::unexpected(
            ReqloomError{ErrorCode::SessionRefreshFailed,
                         ErrorClass::Auth,
                         "refresh: extraction failed: " + values.error().detail});
    }
    return std::move(*values);
}

}  // namespace reqloom::engine
