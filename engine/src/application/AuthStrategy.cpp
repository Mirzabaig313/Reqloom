// Authenticator — strategy dispatch and concrete implementations.
#include "AuthStrategy.h"
#include "JsonExtraction.h"

#include "../domain/Codecs.h"
#include "../domain/VariableResolver.h"
#include "../infrastructure/http/HttpClient.h"

#include <chainapi/engine/Actor.h>

#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace chainapi::engine {

namespace {

using namespace codecs;

/// Implements `AuthStrategy::Simple` and `AuthStrategy::Chain`. Walks
/// `actor.authSteps` in order; each step's response can extract variables
/// that subsequent steps (or the actor's `inject` block) reference.
///
/// All auth failures surface as `SessionRefreshFailed` regardless of root
/// cause — integration tests assert on that single code.
class ChainAuthenticator final : public Authenticator {
public:
    explicit ChainAuthenticator(AuthDependencies deps) : deps_(deps) {}

    std::expected<ActorSession, ChainApiError>
    authenticate(const Actor& actor,
                 const RunContext& ctx,
                 const ResolveContext& rctx) override {
        // nullptr here means a programming error in the engine, not a
        // user-triggerable condition.
        if (!deps_.http || !deps_.varResolver) {
            return std::unexpected(ChainApiError{
                ErrorCode::SessionRefreshFailed, ErrorClass::Auth,
                "auth: authenticator wired without dependencies"});
        }

        ActorSession session;
        session.state = ActorSession::State::Authenticating;

        for (const auto& step : actor.authSteps) {
            auto resolvedPath =
                deps_.varResolver->resolve(step.pathTemplate, ctx, rctx);
            if (!resolvedPath.unresolved.empty()) {
                return std::unexpected(ChainApiError{
                    ErrorCode::SessionRefreshFailed, ErrorClass::Auth,
                    "auth: unresolved variable in path: " +
                    resolvedPath.unresolved.front()});
            }

            HttpRequest req;
            req.method = step.method;
            const auto baseUrlIt = rctx.envVars.find("baseUrl");
            const std::string baseUrl =
                baseUrlIt != rctx.envVars.end() ? baseUrlIt->second : "";
            req.url = baseUrl + resolvedPath.output;

            for (const auto& [k, v] : step.headers) {
                auto resolved = deps_.varResolver->resolve(v, ctx, rctx);
                req.headers[k] = resolved.output;
            }

            if (step.bodyTemplate) {
                auto resolved =
                    deps_.varResolver->resolve(*step.bodyTemplate, ctx, rctx);
                req.body = resolved.output;
                if (!req.headers.contains("Content-Type")) {
                    req.headers["Content-Type"] = "application/json";
                }
            }

            auto response = deps_.http->send(req);
            if (!response) {
                return std::unexpected(ChainApiError{
                    ErrorCode::SessionRefreshFailed, ErrorClass::Auth,
                    "auth: network error during step '" + step.id + "'"});
            }

            if (step.expectStatus && response->status != *step.expectStatus) {
                return std::unexpected(ChainApiError{
                    ErrorCode::SessionRefreshFailed, ErrorClass::Auth,
                    "auth: step '" + step.id + "' returned HTTP " +
                    std::to_string(response->status) + " (expected " +
                    std::to_string(*step.expectStatus) + ")"});
            }

            if (!response->body.empty() && !step.extractions.empty()) {
                auto values = extractFromJson(response->body, step.extractions);
                if (!values) {
                    return std::unexpected(ChainApiError{
                        ErrorCode::SessionRefreshFailed, ErrorClass::Auth,
                        "auth: extraction failed in step '" + step.id +
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

/// Implements `AuthStrategy::Basic`. RFC 7617 HTTP Basic auth: pre-computes
/// `base64(username:password)` and exposes it as `session.variables["credential"]`.
/// No HTTP call is made.
class BasicAuthenticator final : public Authenticator {
public:
    explicit BasicAuthenticator(AuthDependencies deps) : deps_(deps) {}

    std::expected<ActorSession, ChainApiError>
    authenticate(const Actor& actor,
                 const RunContext& ctx,
                 const ResolveContext& rctx) override {
        if (!deps_.varResolver) {
            return std::unexpected(ChainApiError{
                ErrorCode::SessionRefreshFailed, ErrorClass::Auth,
                "auth: basic authenticator wired without resolver"});
        }

        const auto userIt = actor.authConfig.find("username");
        const auto passIt = actor.authConfig.find("password");
        if (userIt == actor.authConfig.end() ||
            passIt == actor.authConfig.end()) {
            return std::unexpected(ChainApiError{
                ErrorCode::SessionRefreshFailed, ErrorClass::Auth,
                "auth: basic strategy requires `username` and `password` "
                "under actor.auth"});
        }

        auto userResolved =
            deps_.varResolver->resolve(userIt->second, ctx, rctx);
        if (!userResolved.unresolved.empty()) {
            return std::unexpected(ChainApiError{
                ErrorCode::SessionRefreshFailed, ErrorClass::Auth,
                "auth: basic.username has unresolved variable: " +
                userResolved.unresolved.front()});
        }
        auto passResolved =
            deps_.varResolver->resolve(passIt->second, ctx, rctx);
        if (!passResolved.unresolved.empty()) {
            return std::unexpected(ChainApiError{
                ErrorCode::SessionRefreshFailed, ErrorClass::Auth,
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
    explicit ApiKeyAuthenticator(AuthDependencies deps) : deps_(deps) {}

    std::expected<ActorSession, ChainApiError>
    authenticate(const Actor& actor,
                 const RunContext& ctx,
                 const ResolveContext& rctx) override {
        if (!deps_.varResolver) {
            return std::unexpected(ChainApiError{
                ErrorCode::SessionRefreshFailed, ErrorClass::Auth,
                "auth: api_key authenticator wired without resolver"});
        }

        const auto keyIt = actor.authConfig.find("key");
        if (keyIt == actor.authConfig.end()) {
            return std::unexpected(ChainApiError{
                ErrorCode::SessionRefreshFailed, ErrorClass::Auth,
                "auth: api_key strategy requires `key` under actor.auth"});
        }
        const auto resolvedKey = deps_.varResolver->resolve(
            keyIt->second, ctx, rctx);
        if (!resolvedKey.unresolved.empty()) {
            return std::unexpected(ChainApiError{
                ErrorCode::SessionRefreshFailed, ErrorClass::Auth,
                "auth: api_key.key has unresolved variable: " +
                resolvedKey.unresolved.front()});
        }

        ActorSession session;
        session.state = ActorSession::State::Authenticating;
        session.variables["key"] = resolvedKey.output;

        // Auto-inject only fires when the user opted in by setting both
        // `location` and `name`.
        const auto locIt  = actor.authConfig.find("location");
        const auto nameIt = actor.authConfig.find("name");
        if (locIt != actor.authConfig.end() &&
            nameIt != actor.authConfig.end()) {
            const auto& location = locIt->second;
            if (location == "header") {
                session.injectHeaders[nameIt->second] = resolvedKey.output;
            } else if (location == "query") {
                session.injectQueryParams[nameIt->second] = resolvedKey.output;
            } else if (location == "cookie") {
                return std::unexpected(ChainApiError{
                    ErrorCode::SessionRefreshFailed, ErrorClass::Auth,
                    "auth: api_key location 'cookie' is not yet supported "
                    "(use a manual inject: header with Cookie: ...)"});
            } else {
                return std::unexpected(ChainApiError{
                    ErrorCode::SessionRefreshFailed, ErrorClass::Auth,
                    "auth: api_key.location must be 'header', 'query', "
                    "or 'cookie'; got: " + location});
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
std::expected<ActorSession, ChainApiError>
executeOAuth2TokenRequest(HttpClient& http,
                          const std::string& tokenUrl,
                          std::string formBody,
                          std::string_view strategyLabel) {
    HttpRequest req;
    req.method = HttpMethod::Post;
    req.url = tokenUrl;
    req.headers["Content-Type"] = "application/x-www-form-urlencoded";
    req.headers["Accept"]       = "application/json";
    req.body = std::move(formBody);

    const auto response = http.send(req);
    if (!response) {
        return std::unexpected(ChainApiError{
            ErrorCode::SessionRefreshFailed, ErrorClass::Auth,
            "auth: " + std::string{strategyLabel} + " network error: " +
            response.error().detail});
    }
    if (response->status < 200 || response->status >= 300) {
        // Surface enough of the body to debug misconfigured credentials.
        constexpr std::size_t kBodyExcerpt = 200;
        std::string excerpt = response->body.size() > kBodyExcerpt
            ? response->body.substr(0, kBodyExcerpt) + "..."
            : response->body;
        return std::unexpected(ChainApiError{
            ErrorCode::SessionRefreshFailed, ErrorClass::Auth,
            "auth: " + std::string{strategyLabel} +
            " token endpoint returned HTTP " +
            std::to_string(response->status) + " — " + excerpt});
    }

    std::vector<Extraction> wanted;
    wanted.push_back({"access_token", "$.access_token",
                      Extraction::Source::JsonPath});
    const auto required = extractFromJson(response->body, wanted);
    if (!required) {
        return std::unexpected(ChainApiError{
            ErrorCode::SessionRefreshFailed, ErrorClass::Auth,
            "auth: " + std::string{strategyLabel} +
            " response missing `access_token`: " + required.error().detail});
    }

    ActorSession session;
    session.state = ActorSession::State::Authenticating;
    const auto& accessToken = required->at("access_token");
    session.variables["access_token"] = accessToken;

    // Optional fields — captured one-at-a-time so a missing `expires_in`
    // doesn't suppress `token_type`.
    for (const auto* optionalField :
         {"expires_in", "token_type", "scope", "refresh_token"}) {
        std::vector<Extraction> opt;
        opt.push_back({optionalField,
                       std::string{"$."} + optionalField,
                       Extraction::Source::JsonPath});
        if (auto extracted = extractFromJson(response->body, opt); extracted) {
            session.variables[optionalField] = extracted->at(optionalField);
        }
    }

    session.injectHeaders["Authorization"] = "Bearer " + accessToken;
    return session;
}

/// Resolve `authConfig[fieldName]` through the variable resolver.
/// Empty strings are treated as missing.
std::expected<std::string, ChainApiError>
resolveAuthConfigField(const Actor& actor,
                       const RunContext& ctx,
                       const ResolveContext& rctx,
                       VariableResolver& resolver,
                       std::string_view strategyLabel,
                       std::string_view fieldName) {
    const auto it = actor.authConfig.find(std::string{fieldName});
    if (it == actor.authConfig.end() || it->second.empty()) {
        return std::unexpected(ChainApiError{
            ErrorCode::SessionRefreshFailed, ErrorClass::Auth,
            "auth: " + std::string{strategyLabel} + " requires `" +
            std::string{fieldName} + "` under actor.auth"});
    }
    auto resolved = resolver.resolve(it->second, ctx, rctx);
    if (!resolved.unresolved.empty()) {
        return std::unexpected(ChainApiError{
            ErrorCode::SessionRefreshFailed, ErrorClass::Auth,
            "auth: " + std::string{strategyLabel} + "." +
            std::string{fieldName} +
            " has unresolved variable: " + resolved.unresolved.front()});
    }
    return resolved.output;
}

/// Resolve an optional config field. Returns empty string when absent.
std::expected<std::string, ChainApiError>
resolveAuthConfigOptional(const Actor& actor,
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
        return std::unexpected(ChainApiError{
            ErrorCode::SessionRefreshFailed, ErrorClass::Auth,
            "auth: " + std::string{strategyLabel} + "." +
            std::string{fieldName} +
            " has unresolved variable: " + resolved.unresolved.front()});
    }
    return resolved.output;
}

/// Implements `AuthStrategy::OAuth2ClientCredentials` (RFC 6749 §4.4).
/// POSTs `grant_type=client_credentials` to `token_url`, extracts
/// `access_token`, and auto-injects `Authorization: Bearer <token>`.
///
/// Reads from `actor.authConfig`:
///   - `token_url`, `client_id`, `client_secret` (required)
///   - `scope` (optional)
class OAuth2ClientCredentialsAuthenticator final : public Authenticator {
public:
    explicit OAuth2ClientCredentialsAuthenticator(AuthDependencies deps)
        : deps_(deps) {}

    std::expected<ActorSession, ChainApiError>
    authenticate(const Actor& actor,
                 const RunContext& ctx,
                 const ResolveContext& rctx) override {
        if (!deps_.http || !deps_.varResolver) {
            return std::unexpected(ChainApiError{
                ErrorCode::SessionRefreshFailed, ErrorClass::Auth,
                "auth: oauth2_client_credentials authenticator wired "
                "without dependencies"});
        }
        constexpr std::string_view kLabel = "oauth2_client_credentials";

        const auto tokenUrl     = resolveAuthConfigField(
            actor, ctx, rctx, *deps_.varResolver, kLabel, "token_url");
        if (!tokenUrl) return std::unexpected(tokenUrl.error());
        const auto clientId     = resolveAuthConfigField(
            actor, ctx, rctx, *deps_.varResolver, kLabel, "client_id");
        if (!clientId) return std::unexpected(clientId.error());
        const auto clientSecret = resolveAuthConfigField(
            actor, ctx, rctx, *deps_.varResolver, kLabel, "client_secret");
        if (!clientSecret) return std::unexpected(clientSecret.error());
        const auto scope = resolveAuthConfigOptional(
            actor, ctx, rctx, *deps_.varResolver, kLabel, "scope");
        if (!scope) return std::unexpected(scope.error());

        // RFC 6749 §4.4.2.
        std::string body =
            "grant_type=client_credentials"
            "&client_id="     + urlEncode(*clientId) +
            "&client_secret=" + urlEncode(*clientSecret);
        if (!scope->empty()) {
            body += "&scope=" + urlEncode(*scope);
        }

        return executeOAuth2TokenRequest(
            *deps_.http, *tokenUrl, std::move(body), kLabel);
    }

private:
    AuthDependencies deps_;
};

/// Implements `AuthStrategy::OAuth2Password` (RFC 6749 §4.3 — resource
/// owner password credentials grant). Same wire shape as
/// `oauth2_client_credentials` but uses `grant_type=password` and
/// includes the resource-owner's `username`/`password`.
///
/// Reads from `actor.authConfig`:
///   - `token_url`, `client_id`, `client_secret`, `username`, `password` (required)
///   - `scope` (optional)
///
/// RFC 6749 §4.3 requires high trust between client and authorization
/// server; prefer the authorization-code flow where possible.
class OAuth2PasswordAuthenticator final : public Authenticator {
public:
    explicit OAuth2PasswordAuthenticator(AuthDependencies deps)
        : deps_(deps) {}

    std::expected<ActorSession, ChainApiError>
    authenticate(const Actor& actor,
                 const RunContext& ctx,
                 const ResolveContext& rctx) override {
        if (!deps_.http || !deps_.varResolver) {
            return std::unexpected(ChainApiError{
                ErrorCode::SessionRefreshFailed, ErrorClass::Auth,
                "auth: oauth2_password authenticator wired without "
                "dependencies"});
        }
        constexpr std::string_view kLabel = "oauth2_password";

        const auto tokenUrl     = resolveAuthConfigField(
            actor, ctx, rctx, *deps_.varResolver, kLabel, "token_url");
        if (!tokenUrl) return std::unexpected(tokenUrl.error());
        const auto clientId     = resolveAuthConfigField(
            actor, ctx, rctx, *deps_.varResolver, kLabel, "client_id");
        if (!clientId) return std::unexpected(clientId.error());
        const auto clientSecret = resolveAuthConfigField(
            actor, ctx, rctx, *deps_.varResolver, kLabel, "client_secret");
        if (!clientSecret) return std::unexpected(clientSecret.error());
        const auto username     = resolveAuthConfigField(
            actor, ctx, rctx, *deps_.varResolver, kLabel, "username");
        if (!username) return std::unexpected(username.error());
        const auto password     = resolveAuthConfigField(
            actor, ctx, rctx, *deps_.varResolver, kLabel, "password");
        if (!password) return std::unexpected(password.error());
        const auto scope = resolveAuthConfigOptional(
            actor, ctx, rctx, *deps_.varResolver, kLabel, "scope");
        if (!scope) return std::unexpected(scope.error());

        // RFC 6749 §4.3.2.
        std::string body =
            "grant_type=password"
            "&username="      + urlEncode(*username) +
            "&password="      + urlEncode(*password) +
            "&client_id="     + urlEncode(*clientId) +
            "&client_secret=" + urlEncode(*clientSecret);
        if (!scope->empty()) {
            body += "&scope=" + urlEncode(*scope);
        }

        return executeOAuth2TokenRequest(
            *deps_.http, *tokenUrl, std::move(body), kLabel);
    }

private:
    AuthDependencies deps_;
};

/// Implements `AuthStrategy::OAuth1` (RFC 5849, two-legged HMAC-SHA1).
/// Unlike OAuth2/Basic/API key, OAuth1 signs per-request. This authenticator:
///   1. Resolves credential fields from `actor.authConfig`.
///   2. Stashes them on the session.
///   3. Sets `session.signingScheme = OAuth1HmacSha1` so the executor
///      calls `signOAuth1Request` before each outbound request.
///
/// Reads from `actor.authConfig`:
///   - `consumer_key`, `consumer_secret` (required)
///   - `token`, `token_secret` (optional, must come as a pair)
///   - `realm` (optional)
class OAuth1Authenticator final : public Authenticator {
public:
    explicit OAuth1Authenticator(AuthDependencies deps) : deps_(deps) {}

    std::expected<ActorSession, ChainApiError>
    authenticate(const Actor& actor,
                 const RunContext& ctx,
                 const ResolveContext& rctx) override {
        if (!deps_.varResolver) {
            return std::unexpected(ChainApiError{
                ErrorCode::SessionRefreshFailed, ErrorClass::Auth,
                "auth: oauth1 authenticator wired without resolver"});
        }
        constexpr std::string_view kLabel = "oauth1";

        const auto consumerKey = resolveAuthConfigField(
            actor, ctx, rctx, *deps_.varResolver, kLabel, "consumer_key");
        if (!consumerKey) return std::unexpected(consumerKey.error());
        const auto consumerSecret = resolveAuthConfigField(
            actor, ctx, rctx, *deps_.varResolver, kLabel, "consumer_secret");
        if (!consumerSecret) return std::unexpected(consumerSecret.error());

        const auto token = resolveAuthConfigOptional(
            actor, ctx, rctx, *deps_.varResolver, kLabel, "token");
        if (!token) return std::unexpected(token.error());
        const auto tokenSecret = resolveAuthConfigOptional(
            actor, ctx, rctx, *deps_.varResolver, kLabel, "token_secret");
        if (!tokenSecret) return std::unexpected(tokenSecret.error());
        const auto realm = resolveAuthConfigOptional(
            actor, ctx, rctx, *deps_.varResolver, kLabel, "realm");
        if (!realm) return std::unexpected(realm.error());

        // Token + token_secret must come as a pair (RFC 5849 §3.1).
        if (token->empty() != tokenSecret->empty()) {
            return std::unexpected(ChainApiError{
                ErrorCode::SessionRefreshFailed, ErrorClass::Auth,
                "auth: oauth1 requires `token` and `token_secret` to be "
                "set together (or both omitted for two-legged signing)"});
        }

        ActorSession session;
        session.state = ActorSession::State::Authenticating;
        session.variables["consumer_key"]    = *consumerKey;
        session.variables["consumer_secret"] = *consumerSecret;
        if (!token->empty())       session.variables["token"]        = *token;
        if (!tokenSecret->empty()) session.variables["token_secret"] = *tokenSecret;
        if (!realm->empty())       session.variables["realm"]        = *realm;
        session.signingScheme = ActorSession::SigningScheme::OAuth1HmacSha1;
        return session;
    }

private:
    AuthDependencies deps_;
};

/// Implements `AuthStrategy::AwsSigV4`. Like OAuth1, AWS SigV4 signs
/// per-request — the signature depends on the URL, method, headers,
/// and body. The authenticator stashes credentials on the session and
/// flips the signing scheme; the executor calls `signSigV4Request`
/// before each outbound HTTP send.
///
/// Reads from `actor.authConfig`:
///   - `access_key`, `secret_key`, `region`, `service` (required)
///   - `session_token` (optional, for STS temporary credentials)
///   - `sign_payload`  (optional, "true" to add `x-amz-content-sha256`;
///                     S3 requires it, most others don't care)
///
/// Per the IAM Best Practices guide, prefer environment-injected
/// short-lived credentials over long-lived access keys committed to
/// chainapi.yaml. The {{X.y}} resolver lets you pull credentials from
/// secret stores at run time.
class AwsSigV4Authenticator final : public Authenticator {
public:
    explicit AwsSigV4Authenticator(AuthDependencies deps) : deps_(deps) {}

    std::expected<ActorSession, ChainApiError>
    authenticate(const Actor& actor,
                 const RunContext& ctx,
                 const ResolveContext& rctx) override {
        if (!deps_.varResolver) {
            return std::unexpected(ChainApiError{
                ErrorCode::SessionRefreshFailed, ErrorClass::Auth,
                "auth: aws_sigv4 authenticator wired without resolver"});
        }
        constexpr std::string_view kLabel = "aws_sigv4";

        const auto accessKey = resolveAuthConfigField(
            actor, ctx, rctx, *deps_.varResolver, kLabel, "access_key");
        if (!accessKey) return std::unexpected(accessKey.error());
        const auto secretKey = resolveAuthConfigField(
            actor, ctx, rctx, *deps_.varResolver, kLabel, "secret_key");
        if (!secretKey) return std::unexpected(secretKey.error());
        const auto region = resolveAuthConfigField(
            actor, ctx, rctx, *deps_.varResolver, kLabel, "region");
        if (!region) return std::unexpected(region.error());
        const auto service = resolveAuthConfigField(
            actor, ctx, rctx, *deps_.varResolver, kLabel, "service");
        if (!service) return std::unexpected(service.error());

        const auto sessionToken = resolveAuthConfigOptional(
            actor, ctx, rctx, *deps_.varResolver, kLabel, "session_token");
        if (!sessionToken) return std::unexpected(sessionToken.error());
        const auto signPayload = resolveAuthConfigOptional(
            actor, ctx, rctx, *deps_.varResolver, kLabel, "sign_payload");
        if (!signPayload) return std::unexpected(signPayload.error());

        ActorSession session;
        session.state = ActorSession::State::Authenticating;
        session.variables["access_key"] = *accessKey;
        session.variables["secret_key"] = *secretKey;
        session.variables["region"]     = *region;
        session.variables["service"]    = *service;
        if (!sessionToken->empty()) {
            session.variables["session_token"] = *sessionToken;
        }
        if (!signPayload->empty()) {
            session.variables["sign_payload"] = *signPayload;
        }
        session.signingScheme = ActorSession::SigningScheme::AwsSigV4;
        return session;
    }

private:
    AuthDependencies deps_;
};

}  // namespace

std::unique_ptr<Authenticator>
selectAuthenticator(const Actor& actor, AuthDependencies deps) {
    switch (actor.strategy) {
        case AuthStrategy::Simple:
        case AuthStrategy::Chain:
            return std::make_unique<ChainAuthenticator>(deps);
        case AuthStrategy::Basic:
            return std::make_unique<BasicAuthenticator>(deps);
        case AuthStrategy::ApiKey:
            return std::make_unique<ApiKeyAuthenticator>(deps);
        case AuthStrategy::OAuth2ClientCredentials:
            return std::make_unique<OAuth2ClientCredentialsAuthenticator>(deps);
        case AuthStrategy::OAuth2Password:
            return std::make_unique<OAuth2PasswordAuthenticator>(deps);
        case AuthStrategy::OAuth1:
            return std::make_unique<OAuth1Authenticator>(deps);
        case AuthStrategy::AwsSigV4:
            return std::make_unique<AwsSigV4Authenticator>(deps);
    }
    return nullptr;
}

}  // namespace chainapi::engine
