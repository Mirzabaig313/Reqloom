// Authenticator — 

#include "AuthStrategy.h"
#include "JsonExtraction.h"

#include "../domain/VariableResolver.h"
#include "../infrastructure/http/HttpClient.h"

#include <chainapi/engine/Actor.h>

#include <cstdint>
#include <cstdio>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace chainapi::engine {

namespace {

// ─── base64 (RFC 4648, standard alphabet, with padding) ─────────────────────
//
// TODO: this duplicates `base64Encode` in
// engine/src/domain/VariableResolver.cpp. The same urlEncode helper
// below is also a third copy of code in `ExecutionEngine.cpp` and
// `VariableResolver.cpp`. Slice 5b will introduce a shared
// `engine/src/util/Codecs.{h,cpp}` and these three call sites will
// migrate together (no behaviour change, just consolidation).
constexpr std::string_view kBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(std::string_view input) {
    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);

    std::size_t i = 0;
    while (i + 3 <= input.size()) {
        const auto b0 = static_cast<std::uint8_t>(input[i]);
        const auto b1 = static_cast<std::uint8_t>(input[i + 1]);
        const auto b2 = static_cast<std::uint8_t>(input[i + 2]);
        out.push_back(kBase64Alphabet[(b0 >> 2) & 0x3F]);
        out.push_back(kBase64Alphabet[((b0 << 4) | (b1 >> 4)) & 0x3F]);
        out.push_back(kBase64Alphabet[((b1 << 2) | (b2 >> 6)) & 0x3F]);
        out.push_back(kBase64Alphabet[b2 & 0x3F]);
        i += 3;
    }
    if (i < input.size()) {
        const auto b0 = static_cast<std::uint8_t>(input[i]);
        out.push_back(kBase64Alphabet[(b0 >> 2) & 0x3F]);
        if (i + 1 == input.size()) {
            out.push_back(kBase64Alphabet[(b0 << 4) & 0x3F]);
            out.push_back('=');
            out.push_back('=');
        } else {
            const auto b1 = static_cast<std::uint8_t>(input[i + 1]);
            out.push_back(kBase64Alphabet[((b0 << 4) | (b1 >> 4)) & 0x3F]);
            out.push_back(kBase64Alphabet[(b1 << 2) & 0x3F]);
            out.push_back('=');
        }
    }
    return out;
}

/// RFC 3986 unreserved characters pass through; everything else is
/// %HH-encoded. Used to build form bodies and query strings for
/// OAuth2 token requests. (See TODO at top of file re consolidation.)
std::string urlEncode(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (const char rawChar : in) {
        const auto c = static_cast<unsigned char>(rawChar);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
            || (c >= '0' && c <= '9')
            || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out.append(buf, 3);
        }
    }
    return out;
}

/// Implements `AuthStrategy::Simple` and `AuthStrategy::Chain`. Walks
/// `actor.authSteps` in order; each step's response can extract
/// variables that subsequent steps (or the actor's `inject` block)
/// reference.
///
/// Failure semantics match the pre-refactor `ensureSession`:
///   - unresolved variable in path  → SessionRefreshFailed
///   - HTTP error                   → SessionRefreshFailed (with detail)
///   - status-code mismatch         → SessionRefreshFailed (with detail)
///   - extraction failure           → SessionRefreshFailed (with detail)
///
/// The single error code is preserved deliberately: integration tests
/// assert on `SessionRefreshFailed` for any auth failure.
class ChainAuthenticator final : public Authenticator {
public:
    explicit ChainAuthenticator(AuthDependencies deps) : deps_(deps) {}

    std::expected<ActorSession, ChainApiError>
    authenticate(const Actor& actor,
                 const RunContext& ctx,
                 const ResolveContext& rctx) override {
        // Defensive: deps must be wired by selectAuthenticator. nullptr
        // here means a programming error in the engine, not a runtime
        // condition the user could trigger.
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

/// Implements `AuthStrategy::Basic`. RFC 7617 HTTP Basic auth: pre-
/// computes `base64(username:password)` and exposes it as the session
/// variable `credential`. The actor's `inject:` block typically sets
/// `Authorization: "Basic {{<actor>.credential}}"` so the encoded
/// value flows automatically into every operation owned by the actor.
///
/// No HTTP call is made — the strategy is pure compute. `username` and
/// `password` come from `actor.authConfig` and are resolved through
/// the variable resolver so secrets and env references work
/// (`username: "{{secret.API_USER}}"`).
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

/// Implements `AuthStrategy::ApiKey` (PRD §5.10.1). Pure compute — no
/// HTTP call. Reads three keys from `actor.authConfig`:
///   - `key`      (required) — the secret token, may contain {{X.y}}
///   - `location` (optional) — "header" | "query" | "cookie"
///   - `name`     (optional, required when `location` set) — sink name
///                e.g. "X-API-Key" or "api_key"
///
/// Hybrid behaviour:
///   - The resolved key is always stored as `session.variables["key"]`,
///     so a fully-explicit config (manual `inject:` block) keeps working.
///   - When `location` and `name` are both set, the strategy ALSO
///     populates the session's `injectHeaders` / `injectQueryParams`
///     so the engine merges the value into every request automatically.
///     One-liner config is enough for the common case; explicit inject
///     remains the escape hatch for non-standard shapes.
///
/// Cookie location is rejected for now — a proper cookie jar is
/// post-MVP. Users with cookie-based API keys can fall through to the
/// manual variable-only form and set `Cookie:` themselves via inject.
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

        // Auto-inject branch: only fires when the user opted in by
        // setting both `location` and `name`. Either is missing → no
        // auto-inject, user is expected to wire `inject:` manually.
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

/// Implements `AuthStrategy::OAuth2ClientCredentials` (RFC 6749 §4.4).
/// POSTs `grant_type=client_credentials` (plus client_id, client_secret,
/// optional scope) to `token_url` as `application/x-www-form-urlencoded`,
/// extracts `access_token` from the JSON response, stores it as
/// `session.variables["access_token"]`, and auto-injects
/// `Authorization: Bearer <token>` into every operation owned by the
/// actor.
///
/// Reads from `actor.authConfig`:
///   - `token_url`     (required) — full URL to the token endpoint
///   - `client_id`     (required) — may contain {{X.y}}
///   - `client_secret` (required) — may contain {{X.y}}
///   - `scope`         (optional) — space-separated scope list
///
/// Failure modes (all surface as `SessionRefreshFailed`):
///   - missing required field
///   - unresolved variable in any field
///   - HTTP error from token endpoint
///   - non-2xx response from token endpoint
///   - response body isn't valid JSON
///   - response JSON missing `access_token`
/// Shared OAuth2 token-endpoint flow used by `OAuth2ClientCredentials`
/// and `OAuth2Password`. Both grants speak the same wire format; the
/// authenticators differ only in which form fields they put in the body.
/// Centralising the POST → JSON extract → session populate → Bearer
/// auto-inject path keeps semantics consistent and prevents subtle
/// divergence between the two classes.
///
/// `formBody` is the already-built `application/x-www-form-urlencoded`
/// payload (caller responsibility — different grants want different
/// fields). `strategyLabel` is folded into error messages so the user
/// sees `oauth2_password token endpoint returned HTTP 400 ...` rather
/// than a generic OAuth2 error.
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
        // RFC 6749 §5.2 token-endpoint error responses use 400 with an
        // `error` field. Surface enough of the body to debug
        // misconfigured credentials / wrong scope / etc.
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

    // Required: access_token. Reuses extractFromJson so error semantics
    // match Simple/Chain.
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
    // doesn't suppress `token_type`. Refresh-token capture is the
    // foundation for future TTL-aware refresh handling (a session.ttl
    // shorter than the token's expires_in is the contract today).
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

    // Auto-inject Bearer token. Same channel as api_key (Slice 4c).
    session.injectHeaders["Authorization"] = "Bearer " + accessToken;
    return session;
}

/// Resolve `authConfig[fieldName]` through the variable resolver.
/// Empty strings are treated as missing — the user gets a clean error
/// before the strategy ever calls the token endpoint.
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

/// Resolve an optional config field. Returns empty string when the
/// field is absent or empty; surfaces resolver errors when present.
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
/// `oauth2_client_credentials` but the form body uses
/// `grant_type=password` and includes the resource-owner's
/// `username`/`password` alongside the client credentials.
///
/// Reads from `actor.authConfig`:
///   - `token_url`     (required)
///   - `client_id`     (required) — resolved via {{X.y}}
///   - `client_secret` (required) — resolved via {{X.y}}
///   - `username`      (required) — the resource-owner identity
///   - `password`      (required) — resource-owner password (resolve
///                                  via `{{secret.X}}` in practice)
///   - `scope`         (optional)
///
/// Same auto-injection behaviour as client_credentials (Bearer header)
/// and same error surface.
///
/// SECURITY NOTE: §4.3 requires "the client and authorization server
/// have a high degree of trust"; the grant exists primarily for
/// migrating legacy auth schemes. Modern apps should prefer the
/// authorization-code flow (Slice 4h, deferred) where possible.
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
    }
    // Future strategies (PRD §5.10.1) extend the AuthStrategy enum and
    // get a case here. The engine surfaces an unmatched value as
    // SessionRefreshFailed via the nullptr return.
    return nullptr;
}

}  // namespace chainapi::engine
