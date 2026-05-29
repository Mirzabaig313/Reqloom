// Authenticator — strategy dispatch and concrete implementations.
#include "AuthStrategy.h"
#include "Cookies.h"
#include "HeaderMasking.h"
#include "JsonExtraction.h"

#include "../domain/Codecs.h"
#include "../domain/VariableResolver.h"
#include "../infrastructure/http/HttpClient.h"

#include <chainapi/engine/Actor.h>
#include <chainapi/engine/Events.h>
#include <chainapi/engine/ExecutionEngine.h>  // kMaxCapturedBodyBytes

#include <nlohmann/json.hpp>

#include <chrono>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace chainapi::engine {

namespace {

using namespace codecs;
using json = nlohmann::json;

// Masked headers as an ordered vector for event emission.
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

// Opt-in body payload for an auth-flow `ResponseReceived`. Returns nullopt
// unless capture is on; caps at `kMaxCapturedBodyBytes`. Auth bodies carry
// tokens — the caller gates this behind RunOptions::captureResponseBodies.
[[nodiscard]] std::optional<std::string> capturedBody(const std::string& body, bool capture) {
    if (!capture) {
        return std::nullopt;
    }
    if (body.size() > kMaxCapturedBodyBytes) {
        return body.substr(0, kMaxCapturedBodyBytes);
    }
    return body;
}

// Implements AuthStrategy::Simple and AuthStrategy::Chain. Walks actor.authSteps
// in order; each step's response can extract variables for subsequent steps.
class ChainAuthenticator final : public Authenticator {
public:
    explicit ChainAuthenticator(AuthDependencies deps) : deps_(std::move(deps)) {}

    std::expected<ActorSession, ChainApiError> authenticate(const Actor& actor,
                                                            RunContext& ctx,
                                                            const ResolveContext& rctx) override {
        // nullptr here means a programming error in the engine, not a
        // user-triggerable condition.
        if ((deps_.http == nullptr) || (deps_.varResolver == nullptr)) {
            return std::unexpected(ChainApiError{ErrorCode::SessionRefreshFailed,
                                                 ErrorClass::Auth,
                                                 "auth: authenticator wired without dependencies"});
        }

        ActorSession session;
        session.state = ActorSession::State::Authenticating;

        for (const auto& step : actor.authSteps) {
            auto resolvedPath = deps_.varResolver->resolve(step.pathTemplate, ctx, rctx);
            if (!resolvedPath.unresolved.empty()) {
                return std::unexpected(ChainApiError{
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
                auto resolved = deps_.varResolver->resolve(*step.bodyTemplate, ctx, rctx);
                req.body = resolved.output;
                if (!req.headers.contains("Content-Type")) {
                    req.headers["Content-Type"] = "application/json";
                }
            }

            if (deps_.emit) {
                // runId/stepIndex are the parent step's, so the timeline
                // groups the auth request under the op that triggered it.
                deps_.emit(RequestPrepared{deps_.runId,
                                           deps_.stepIndex,
                                           req.method,
                                           req.url,
                                           snapshotMaskedRequestHeaders(req),
                                           requestBodySize(req),
                                           std::chrono::system_clock::now()});
            }

            auto response = deps_.http->send(req);
            if (!response) {
                return std::unexpected(
                    ChainApiError{ErrorCode::SessionRefreshFailed,
                                  ErrorClass::Auth,
                                  "auth: network error during step '" + step.id + "'"});
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
                                     capturedBody(response->body, deps_.captureResponseBodies)});
            }

            // Absorb Set-Cookie headers from the auth response into the
            // actor's jar so subsequent operations (which run AS this
            // actor) carry the cookie. PHP / Rails apps that issue a
            // session cookie on /login depend on this; without it the
            // cookie disappears the moment authenticate() returns.
            for (const auto& [name, value] : cookies::collectFromResponse(response->headers)) {
                ctx.setCookie(actor.id, name, value);
            }

            if (step.expectStatus && response->status != *step.expectStatus) {
                return std::unexpected(ChainApiError{ErrorCode::SessionRefreshFailed,
                                                     ErrorClass::Auth,
                                                     "auth: step '" + step.id + "' returned HTTP " +
                                                         std::to_string(response->status) +
                                                         " (expected " +
                                                         std::to_string(*step.expectStatus) + ")"});
            }

            if (!response->body.empty() && !step.extractions.empty()) {
                auto values = extractFromJson(response->body, step.extractions);
                if (!values) {
                    return std::unexpected(ChainApiError{ErrorCode::SessionRefreshFailed,
                                                         ErrorClass::Auth,
                                                         "auth: extraction failed in step '" +
                                                             step.id +
                                                             "': " + values.error().detail});
                }
                for (auto& [k, v] : *values) {
                    session.variables[k] = std::move(v);
                }
            }
        }

        return session;
    }

private:
    AuthDependencies deps_;
};

// Implements AuthStrategy::Basic (RFC 7617). Pre-computes base64(username:password)
// and exposes it as session.variables["credential"]. No HTTP call is made.
class BasicAuthenticator final : public Authenticator {
public:
    explicit BasicAuthenticator(AuthDependencies deps) : deps_(std::move(deps)) {}

    std::expected<ActorSession, ChainApiError> authenticate(const Actor& actor,
                                                            RunContext& ctx,
                                                            const ResolveContext& rctx) override {
        if (deps_.varResolver == nullptr) {
            return std::unexpected(
                ChainApiError{ErrorCode::SessionRefreshFailed,
                              ErrorClass::Auth,
                              "auth: basic authenticator wired without resolver"});
        }

        const auto userIt = actor.authConfig.find("username");
        const auto passIt = actor.authConfig.find("password");
        if (userIt == actor.authConfig.end() || passIt == actor.authConfig.end()) {
            return std::unexpected(
                ChainApiError{ErrorCode::SessionRefreshFailed,
                              ErrorClass::Auth,
                              "auth: basic strategy requires `username` and `password` "
                              "under actor.auth"});
        }

        auto userResolved = deps_.varResolver->resolve(userIt->second, ctx, rctx);
        if (!userResolved.unresolved.empty()) {
            return std::unexpected(ChainApiError{ErrorCode::SessionRefreshFailed,
                                                 ErrorClass::Auth,
                                                 "auth: basic.username has unresolved variable: " +
                                                     userResolved.unresolved.front()});
        }
        auto passResolved = deps_.varResolver->resolve(passIt->second, ctx, rctx);
        if (!passResolved.unresolved.empty()) {
            return std::unexpected(ChainApiError{ErrorCode::SessionRefreshFailed,
                                                 ErrorClass::Auth,
                                                 "auth: basic.password has unresolved variable: " +
                                                     passResolved.unresolved.front()});
        }

        ActorSession session;
        session.state = ActorSession::State::Authenticating;
        session.variables["credential"] =
            base64Encode(userResolved.output + ":" + passResolved.output);
        return session;
    }

private:
    AuthDependencies deps_;
};

/// Implements `AuthStrategy::ApiKey`. Pure compute — no HTTP call.
/// Reads from `actor.authConfig`:
///   - `key`      (required) — the secret token, may contain {{X.y}}
///   - `location` (optional) — "header" | "query" | "cookie"
///   - `name`     (optional, required when `location` set) — sink name
///
/// The resolved key is always stored as `session.variables["key"]`.
/// When `location` and `name` are both set, also populates
/// `injectHeaders` / `injectQueryParams` for automatic injection.
/// Cookie location is rejected — cookie jar is post-MVP.
class ApiKeyAuthenticator final : public Authenticator {
public:
    explicit ApiKeyAuthenticator(AuthDependencies deps) : deps_(std::move(deps)) {}

    std::expected<ActorSession, ChainApiError> authenticate(const Actor& actor,
                                                            RunContext& ctx,
                                                            const ResolveContext& rctx) override {
        if (deps_.varResolver == nullptr) {
            return std::unexpected(
                ChainApiError{ErrorCode::SessionRefreshFailed,
                              ErrorClass::Auth,
                              "auth: api_key authenticator wired without resolver"});
        }

        const auto keyIt = actor.authConfig.find("key");
        if (keyIt == actor.authConfig.end()) {
            return std::unexpected(
                ChainApiError{ErrorCode::SessionRefreshFailed,
                              ErrorClass::Auth,
                              "auth: api_key strategy requires `key` under actor.auth"});
        }
        const auto resolvedKey = deps_.varResolver->resolve(keyIt->second, ctx, rctx);
        if (!resolvedKey.unresolved.empty()) {
            return std::unexpected(ChainApiError{
                ErrorCode::SessionRefreshFailed,
                ErrorClass::Auth,
                "auth: api_key.key has unresolved variable: " + resolvedKey.unresolved.front()});
        }

        ActorSession session;
        session.state = ActorSession::State::Authenticating;
        session.variables["key"] = resolvedKey.output;

        // Auto-inject only fires when the user opted in by setting both
        // `location` and `name`.
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
                    ChainApiError{ErrorCode::SessionRefreshFailed,
                                  ErrorClass::Auth,
                                  "auth: api_key location 'cookie' is not yet supported "
                                  "(use a manual inject: header with Cookie: ...)"});
            } else {
                return std::unexpected(
                    ChainApiError{ErrorCode::SessionRefreshFailed,
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

/// Shared OAuth2 token-endpoint flow used by `OAuth2ClientCredentials`
/// and `OAuth2Password`. `formBody` is the already-built
/// `application/x-www-form-urlencoded` payload.
std::expected<ActorSession, ChainApiError> executeOAuth2TokenRequest(
    HttpClient& http,
    const std::string& tokenUrl,
    std::string formBody,
    std::string_view strategyLabel,
    const TransportConfig& transport) {
    HttpRequest req;
    req.method = HttpMethod::Post;
    req.url = tokenUrl;
    req.headers["Content-Type"] = "application/x-www-form-urlencoded";
    req.headers["Accept"] = "application/json";
    req.body = std::move(formBody);
    req.transport = transport;

    const auto response = http.send(req);
    if (!response) {
        return std::unexpected(ChainApiError{
            ErrorCode::SessionRefreshFailed,
            ErrorClass::Auth,
            "auth: " + std::string{strategyLabel} + " network error: " + response.error().detail});
    }
    if (response->status < 200 || response->status >= 300) {
        // Surface enough of the body to debug misconfigured credentials.
        constexpr std::size_t kBodyExcerpt = 200;
        std::string const excerpt = response->body.size() > kBodyExcerpt
                                        ? response->body.substr(0, kBodyExcerpt) + "..."
                                        : response->body;
        return std::unexpected(
            ChainApiError{ErrorCode::SessionRefreshFailed,
                          ErrorClass::Auth,
                          "auth: " + std::string{strategyLabel} + " token endpoint returned HTTP " +
                              std::to_string(response->status) + " — " + excerpt});
    }

    // Parse the token response once and read every field from the single
    // document — the previous approach re-ran extractFromJson (a full
    // json::parse) once per optional field, parsing the body up to 5×.
    json doc;
    try {
        doc = json::parse(response->body);
    } catch (const json::parse_error& e) {
        return std::unexpected(ChainApiError{
            ErrorCode::SessionRefreshFailed,
            ErrorClass::Auth,
            "auth: " + std::string{strategyLabel} + " response is not valid JSON: " + e.what()});
    }

    const auto accessTokenIt = doc.find("access_token");
    if (accessTokenIt == doc.end() || !accessTokenIt->is_string()) {
        return std::unexpected(ChainApiError{
            ErrorCode::SessionRefreshFailed,
            ErrorClass::Auth,
            "auth: " + std::string{strategyLabel} + " response missing string `access_token`"});
    }

    ActorSession session;
    session.state = ActorSession::State::Authenticating;
    auto accessToken = accessTokenIt->get<std::string>();

    // Optional fields — captured one-at-a-time so a missing `expires_in`
    // doesn't suppress `token_type`. A field that is present but not a
    // string is rendered with dump() to match the prior JSONPath behaviour.
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

/// Resolve `authConfig[fieldName]` through the variable resolver.
/// Empty strings are treated as missing.
std::expected<std::string, ChainApiError> resolveAuthConfigField(const Actor& actor,
                                                                 const RunContext& ctx,
                                                                 const ResolveContext& rctx,
                                                                 VariableResolver& resolver,
                                                                 std::string_view strategyLabel,
                                                                 std::string_view fieldName) {
    const auto it = actor.authConfig.find(std::string{fieldName});
    if (it == actor.authConfig.end() || it->second.empty()) {
        return std::unexpected(ChainApiError{ErrorCode::SessionRefreshFailed,
                                             ErrorClass::Auth,
                                             "auth: " + std::string{strategyLabel} + " requires `" +
                                                 std::string{fieldName} + "` under actor.auth"});
    }
    auto resolved = resolver.resolve(it->second, ctx, rctx);
    if (!resolved.unresolved.empty()) {
        return std::unexpected(
            ChainApiError{ErrorCode::SessionRefreshFailed,
                          ErrorClass::Auth,
                          "auth: " + std::string{strategyLabel} + "." + std::string{fieldName} +
                              " has unresolved variable: " + resolved.unresolved.front()});
    }
    return resolved.output;
}

/// Resolve an optional config field. Returns empty string when absent.
std::expected<std::string, ChainApiError> resolveAuthConfigOptional(const Actor& actor,
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
            ChainApiError{ErrorCode::SessionRefreshFailed,
                          ErrorClass::Auth,
                          "auth: " + std::string{strategyLabel} + "." + std::string{fieldName} +
                              " has unresolved variable: " + resolved.unresolved.front()});
    }
    return resolved.output;
}

// Implements AuthStrategy::OAuth2ClientCredentials (RFC 6749 §4.4).
// POSTs grant_type=client_credentials to token_url, extracts access_token,
// auto-injects Authorization: Bearer. Reads token_url/client_id/client_secret
// from actor.authConfig; scope is optional.
class OAuth2ClientCredentialsAuthenticator final : public Authenticator {
public:
    explicit OAuth2ClientCredentialsAuthenticator(AuthDependencies deps) : deps_(std::move(deps)) {}

    std::expected<ActorSession, ChainApiError> authenticate(const Actor& actor,
                                                            RunContext& ctx,
                                                            const ResolveContext& rctx) override {
        if ((deps_.http == nullptr) || (deps_.varResolver == nullptr)) {
            return std::unexpected(
                ChainApiError{ErrorCode::SessionRefreshFailed,
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
        const auto clientSecret =
            resolveAuthConfigField(actor, ctx, rctx, *deps_.varResolver, kLabel, "client_secret");
        if (!clientSecret) {
            return std::unexpected(clientSecret.error());
        }
        const auto scope =
            resolveAuthConfigOptional(actor, ctx, rctx, *deps_.varResolver, kLabel, "scope");
        if (!scope) {
            return std::unexpected(scope.error());
        }

        // RFC 6749 §4.4.2.
        std::string body =
            "grant_type=client_credentials"
            "&client_id=" +
            urlEncode(*clientId) + "&client_secret=" + urlEncode(*clientSecret);
        if (!scope->empty()) {
            body += "&scope=" + urlEncode(*scope);
        }

        return executeOAuth2TokenRequest(
            *deps_.http, *tokenUrl, std::move(body), kLabel, rctx.transport);
    }

private:
    AuthDependencies deps_;
};

// Implements AuthStrategy::OAuth2Password (RFC 6749 §4.3). Same wire shape
// as client_credentials but uses grant_type=password with username/password.
// Reads token_url/client_id/client_secret/username/password from authConfig;
// scope is optional.
class OAuth2PasswordAuthenticator final : public Authenticator {
public:
    explicit OAuth2PasswordAuthenticator(AuthDependencies deps) : deps_(std::move(deps)) {}

    std::expected<ActorSession, ChainApiError> authenticate(const Actor& actor,
                                                            RunContext& ctx,
                                                            const ResolveContext& rctx) override {
        if ((deps_.http == nullptr) || (deps_.varResolver == nullptr)) {
            return std::unexpected(
                ChainApiError{ErrorCode::SessionRefreshFailed,
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
        const auto clientSecret =
            resolveAuthConfigField(actor, ctx, rctx, *deps_.varResolver, kLabel, "client_secret");
        if (!clientSecret) {
            return std::unexpected(clientSecret.error());
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
        const auto scope =
            resolveAuthConfigOptional(actor, ctx, rctx, *deps_.varResolver, kLabel, "scope");
        if (!scope) {
            return std::unexpected(scope.error());
        }

        // RFC 6749 §4.3.2.
        std::string body =
            "grant_type=password"
            "&username=" +
            urlEncode(*username) + "&password=" + urlEncode(*password) +
            "&client_id=" + urlEncode(*clientId) + "&client_secret=" + urlEncode(*clientSecret);
        if (!scope->empty()) {
            body += "&scope=" + urlEncode(*scope);
        }

        return executeOAuth2TokenRequest(
            *deps_.http, *tokenUrl, std::move(body), kLabel, rctx.transport);
    }

private:
    AuthDependencies deps_;
};

// Implements AuthStrategy::OAuth1 (RFC 5849, two-legged HMAC-SHA1).
// Stashes credentials on the session and sets signingScheme = OAuth1HmacSha1
// so the executor calls signOAuth1Request before each outbound request.
// Reads consumer_key/consumer_secret from authConfig; token/token_secret and
// realm are optional.
class OAuth1Authenticator final : public Authenticator {
public:
    explicit OAuth1Authenticator(AuthDependencies deps) : deps_(std::move(deps)) {}

    std::expected<ActorSession, ChainApiError> authenticate(const Actor& actor,
                                                            RunContext& ctx,
                                                            const ResolveContext& rctx) override {
        if (deps_.varResolver == nullptr) {
            return std::unexpected(
                ChainApiError{ErrorCode::SessionRefreshFailed,
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
                ChainApiError{ErrorCode::SessionRefreshFailed,
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

// Implements AuthStrategy::AwsSigV4. Signs per-request — stashes credentials
// on the session and sets signingScheme = AwsSigV4 so the executor calls
// signSigV4Request before each outbound HTTP send.
// Reads access_key/secret_key/region/service from authConfig; session_token
// is optional (for STS temporary credentials).
// Per the IAM Best Practices guide, prefer environment-injected
/// short-lived credentials over long-lived access keys committed to
/// chainapi.yaml. The {{X.y}} resolver lets you pull credentials from
/// secret stores at run time.
class AwsSigV4Authenticator final : public Authenticator {
public:
    explicit AwsSigV4Authenticator(AuthDependencies deps) : deps_(std::move(deps)) {}

    std::expected<ActorSession, ChainApiError> authenticate(const Actor& actor,
                                                            RunContext& ctx,
                                                            const ResolveContext& rctx) override {
        if (deps_.varResolver == nullptr) {
            return std::unexpected(
                ChainApiError{ErrorCode::SessionRefreshFailed,
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
    }
    return nullptr;
}

std::expected<std::map<std::string, std::string>, ChainApiError> runRefresh(
    const Actor& actor, RunContext& ctx, const ResolveContext& rctx, const AuthDependencies& deps) {
    if (!actor.refresh) {
        return std::unexpected(ChainApiError{
            ErrorCode::SessionRefreshFailed,
            ErrorClass::Auth,
            "refresh: actor '" + actor.id.value + "' has no `session.refresh:` block defined"});
    }
    if ((deps.http == nullptr) || (deps.varResolver == nullptr)) {
        return std::unexpected(ChainApiError{
            ErrorCode::SessionRefreshFailed, ErrorClass::Auth, "refresh: dependencies not wired"});
    }

    const auto& refresh = *actor.refresh;
    auto resolvedPath = deps.varResolver->resolve(refresh.pathTemplate, ctx, rctx);
    if (!resolvedPath.unresolved.empty()) {
        return std::unexpected(ChainApiError{
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
        auto resolved = deps.varResolver->resolve(*refresh.bodyTemplate, ctx, rctx);
        req.body = resolved.output;
        if (!req.headers.contains("Content-Type")) {
            req.headers["Content-Type"] = "application/json";
        }
    }

    if (deps.emit) {
        deps.emit(RequestPrepared{deps.runId,
                                  deps.stepIndex,
                                  req.method,
                                  req.url,
                                  snapshotMaskedRequestHeaders(req),
                                  requestBodySize(req),
                                  std::chrono::system_clock::now()});
    }

    auto response = deps.http->send(req);
    if (!response) {
        return std::unexpected(ChainApiError{ErrorCode::SessionRefreshFailed,
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
                                   capturedBody(response->body, deps.captureResponseBodies)});
    }

    // Absorb Set-Cookie headers from the refresh response — same
    // contract as ChainAuthenticator. Refresh endpoints commonly
    // rotate session cookies (CSRF tokens, anti-replay nonces) and
    // the next operation as this actor needs the rotated value.
    for (const auto& [name, value] : cookies::collectFromResponse(response->headers)) {
        ctx.setCookie(actor.id, name, value);
    }

    // Honour the user's `expect_status:` when set, otherwise fall back
    // to "any 2xx is success". Both forms align with the operation-level
    // schema so users can copy patterns between the two.
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
        return std::unexpected(ChainApiError{
            ErrorCode::SessionRefreshFailed,
            ErrorClass::Auth,
            "refresh: HTTP " + std::to_string(response->status) +
                " (refresh endpoint rejected the existing credentials — caller should re-auth)"});
    }

    if (refresh.extractions.empty()) {
        // No declared extractions — treat as "refresh succeeded, but
        // there's nothing to merge". Return an empty map so the caller
        // can update expiresAt without overwriting any session vars.
        return std::map<std::string, std::string>{};
    }
    auto values = extractFromJson(response->body, refresh.extractions);
    if (!values) {
        return std::unexpected(
            ChainApiError{ErrorCode::SessionRefreshFailed,
                          ErrorClass::Auth,
                          "refresh: extraction failed: " + values.error().detail});
    }
    return std::move(*values);
}

}  // namespace chainapi::engine
