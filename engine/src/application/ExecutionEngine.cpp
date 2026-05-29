// ExecutionEngine — resolves dependency chains, authenticates actors, executes steps.

#include <chainapi/engine/ExecutionEngine.h>

#include "../domain/Codecs.h"
#include "../domain/DependencyResolver.h"
#include "../domain/VariableResolver.h"
#include "../infrastructure/hooks/HookRunner.h"
#include "../infrastructure/http/HttpClient.h"
#include "../infrastructure/schema/SchemaParser.h"
#include "../infrastructure/secrets/SecretStore.h"
#include "../infrastructure/storage/HistoryStore.h"
#include "AuthStrategy.h"
#include "Cookies.h"
#include "HeaderMasking.h"
#include "JsonExtraction.h"
#include "MultipartBuilder.h"
#include "PredicateEvaluator.h"
#include "RequestSigners.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <ctime>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace chainapi::engine {

using json = nlohmann::json;

namespace {

using namespace codecs;

/// Build a HookContext snapshot from current run state. Per AGENTS.md
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

    explicit Impl(Dependencies d) : deps(std::move(d)) {}

    [[nodiscard]] bool isCancelled(RunId runId) const noexcept {
        const auto cancelled = cancelledRunId.load(std::memory_order_acquire);
        return cancelled != 0 && cancelled == runId.value;
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

    void emit(const RunEvent& e) {
        // Persist before fanning out — the event survives a subscriber
        // that crashes the process. Best-effort: a persistence failure
        // must never break the run (log + continue once §10 logging lands).
        if (deps.history) {
            // NOLINTNEXTLINE(bugprone-unused-return-value)
            (void)deps.history->append(e);
        }

        // Snapshot before invoking — avoids re-entrant deadlock if a callback calls subscribe().
        std::vector<EventCallback> snapshot;
        {
            const std::lock_guard lock(subscriberMutex);
            snapshot = subscribers;
        }
        for (auto& cb : snapshot) {
            try {
                cb(e);
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
                AuthDependencies refreshDeps{deps.http.get(), &varResolver, sink, runId, stepIndex};
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

        AuthDependencies authDeps{deps.http.get(), &varResolver, sink, runId, stepIndex};
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
    std::expected<HttpResponse, ChainApiError> runPollLoop(const Operation& op,
                                                           const PollUntil& poll,
                                                           const Project& project,
                                                           RunContext& ctx,
                                                           const ResolveContext& rctx,
                                                           RunId runId,
                                                           std::size_t stepIndex,
                                                           const HttpResponse& /*initialResponse*/,
                                                           std::vector<StepResult>& attemptRows) {
        PredicateEvaluator evaluator;

        auto successPredicate = evaluator.parse(poll.successWhen);
        if (!successPredicate) {
            return std::unexpected(
                ChainApiError{ErrorCode::SchemaInvalid,
                              ErrorClass::Schema,
                              "poll_until.success_when: " + successPredicate.error().detail});
        }

        std::optional<ParsedPredicate> failPredicate;
        if (poll.failWhen) {
            auto parsed = evaluator.parse(*poll.failWhen);
            if (!parsed) {
                return std::unexpected(
                    ChainApiError{ErrorCode::SchemaInvalid,
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
            return std::unexpected(ChainApiError{ErrorCode::SessionRefreshFailed,
                                                 ErrorClass::Auth,
                                                 "poll_until: actor session refresh failed"});
        }

        const auto deadline = std::chrono::steady_clock::now() + poll.timeout;
        HttpResponse lastResponse;
        bool haveLastResponse = false;

        for (int attempt = 0; attempt < poll.maxAttempts; ++attempt) {
            if (isCancelled(runId)) {
                return std::unexpected(
                    ChainApiError{ErrorCode::Cancelled, ErrorClass::Run, "poll_until: cancelled"});
            }

            HttpRequest req;
            req.method = poll.method;
            req.transport = rctx.transport;
            auto resolvedPath = varResolver.resolve(poll.pathTemplate, ctx, rctx);
            if (!resolvedPath.unresolved.empty()) {
                return std::unexpected(ChainApiError{
                    ErrorCode::VarUnresolved,
                    ErrorClass::Resolution,
                    "poll_until: unresolved variable in path: " + resolvedPath.unresolved.front()});
            }
            req.url = baseUrl + resolvedPath.output;
            if (pollActor != nullptr) {
                for (const auto& [k, v] : pollActor->inject.headers) {
                    auto resolved = varResolver.resolve(v, ctx, rctx);
                    req.headers[k] = resolved.output;
                }
                // Session-level inject. Session wins on key collision.
                if (const auto* session = ctx.session(pollActor->id); session) {
                    for (const auto& [k, v] : session->injectHeaders) {
                        req.headers[k] = v;
                    }
                    if (!session->injectQueryParams.empty()) {
                        std::string qs;
                        for (const auto& [k, v] : session->injectQueryParams) {
                            if (!qs.empty()) {
                                qs += "&";
                            }
                            qs += urlEncode(k) + "=" + urlEncode(v);
                        }
                        req.url += (req.url.find('?') == std::string::npos ? "?" : "&") + qs;
                    }
                }

                // Cookie jar emission for poll requests. Same priority
                // as the parent op: a poll inheriting the parent's
                // actor sees the jar that has accumulated through the
                // initial response and any prior poll attempt.
                if (!req.headers.contains("Cookie")) {
                    const auto jar = ctx.cookies(pollActor->id);
                    if (!jar.empty()) {
                        req.headers["Cookie"] = cookies::formatRequestHeader(jar);
                    }
                }
            }

            // Per-request signing done after inject merge so the signer sees the final shape.
            if (pollActor != nullptr) {
                if (const auto* session = ctx.session(pollActor->id); session) {
                    if (session->signingScheme == ActorSession::SigningScheme::OAuth1HmacSha1) {
                        if (!signOAuth1Request(req, *session)) {
                            return std::unexpected(
                                ChainApiError{ErrorCode::SessionRefreshFailed,
                                              ErrorClass::Auth,
                                              "poll_until: oauth1 signing failed (missing "
                                              "consumer credentials or malformed URL)"});
                        }
                    } else if (session->signingScheme == ActorSession::SigningScheme::AwsSigV4) {
                        if (!signSigV4Request(req, *session)) {
                            return std::unexpected(
                                ChainApiError{ErrorCode::SessionRefreshFailed,
                                              ErrorClass::Auth,
                                              "poll_until: aws_sigv4 signing failed "
                                              "(missing access_key/secret_key/region/service "
                                              "or malformed URL)"});
                        }
                    }
                }
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
                                  std::chrono::system_clock::now()});

            // Update the cookie jar from this poll's Set-Cookie headers.
            // Pollers that issue stateful status checks (rare but real)
            // can rotate session cookies — without absorbing them here
            // a follow-up op would send a stale cookie.
            if (pollActor != nullptr) {
                absorbResponseCookies(lastResponse.headers, pollActor->id, ctx);
            }

            // Each poll attempt is a timeline row alongside the parent step.
            const auto failMatched =
                failPredicate &&
                evaluator.evaluate(*failPredicate, lastResponse.body, lastResponse.status) ==
                    PredicateValue::True;
            const auto successMatched =
                !failMatched &&
                evaluator.evaluate(*successPredicate, lastResponse.body, lastResponse.status) ==
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
                return std::unexpected(ChainApiError{ErrorCode::PollFailPredicate,
                                                     ErrorClass::Polling,
                                                     "poll_until.fail_when matched (HTTP " +
                                                         std::to_string(lastResponse.status) +
                                                         ")"});
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
                return std::unexpected(ChainApiError{
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

        return std::unexpected(ChainApiError{
            ErrorCode::PollMaxAttemptsExceeded,
            ErrorClass::Polling,
            haveLastResponse
                ? "poll_until: max_attempts (" + std::to_string(poll.maxAttempts) +
                      ") exceeded — last response: HTTP " + std::to_string(lastResponse.status)
                : "poll_until: max_attempts (" + std::to_string(poll.maxAttempts) + ") exceeded"});
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
                           std::vector<StepResult>& pollAttemptRows) {
        StepResult result;
        result.op = op.id;
        result.attempts = 1;
        auto startTime = std::chrono::steady_clock::now();

        if (!op.actor.value.empty()) {
            auto actorIt = project.actors.find(op.actor);
            if (actorIt != project.actors.end()) {
                if (!ensureSession(actorIt->second, ctx, rctx, runId, stepIndex)) {
                    result.status = StepResult::Status::Failed;
                    result.error = ErrorCode::SessionRefreshFailed;
                    return result;
                }
            }
        }

        auto resolvedPath = varResolver.resolve(op.pathTemplate, ctx, rctx);
        if (!resolvedPath.unresolved.empty()) {
            result.status = StepResult::Status::Failed;
            result.error = ErrorCode::VarUnresolved;
            return result;
        }

        HttpRequest req;
        req.method = op.method;
        req.transport = rctx.transport;
        auto baseUrlIt = rctx.envVars.find("baseUrl");
        std::string baseUrl = baseUrlIt != rctx.envVars.end() ? baseUrlIt->second : "";
        req.url = baseUrl + resolvedPath.output;

        for (const auto& [k, v] : op.headers) {
            auto resolved = varResolver.resolve(v, ctx, rctx);
            req.headers[k] = resolved.output;
        }

        if (!op.actor.value.empty()) {
            auto actorIt = project.actors.find(op.actor);
            if (actorIt != project.actors.end()) {
                for (const auto& [k, v] : actorIt->second.inject.headers) {
                    auto resolved = varResolver.resolve(v, ctx, rctx);
                    req.headers[k] = resolved.output;
                }
                // Session-level inject wins on key collision.
                if (const auto* session = ctx.session(op.actor); session) {
                    for (const auto& [k, v] : session->injectHeaders) {
                        req.headers[k] = v;
                    }
                }
            }

            // Cookie jar emission. The actor's jar accumulates from
            // every Set-Cookie the server has sent on prior operations
            // performed AS this actor in the current run. We emit a
            // single `Cookie:` header rolling them up.
            if (!req.headers.contains("Cookie")) {
                const auto jar = ctx.cookies(op.actor);
                if (!jar.empty()) {
                    req.headers["Cookie"] = cookies::formatRequestHeader(jar);
                }
            }
        }

        // Session-level injects folded in, winning on key collision.
        std::map<std::string, std::string> queryParams;
        for (const auto& [k, v] : op.queryParams) {
            auto resolved = varResolver.resolve(v, ctx, rctx);
            queryParams[k] = resolved.output;
        }
        if (!op.actor.value.empty()) {
            if (const auto* session = ctx.session(op.actor); session) {
                for (const auto& [k, v] : session->injectQueryParams) {
                    queryParams[k] = v;
                }
            }
        }
        if (!queryParams.empty()) {
            std::string qs;
            for (const auto& [k, v] : queryParams) {
                if (!qs.empty()) {
                    qs += "&";
                }
                qs += urlEncode(k) + "=" + urlEncode(v);
            }
            req.url += (req.url.find('?') == std::string::npos ? "?" : "&") + qs;
        }

        if (op.bodyTemplate) {
            auto resolved = varResolver.resolve(*op.bodyTemplate, ctx, rctx);
            req.body = resolved.output;
            if (!req.headers.contains("Content-Type")) {
                req.headers["Content-Type"] = "application/json";
            }
        } else if (op.bodyForm) {
            std::map<std::string, std::string> resolvedFields;
            for (const auto& [k, v] : *op.bodyForm) {
                resolvedFields[k] = varResolver.resolve(v, ctx, rctx).output;
            }
            const bool routeMultipart = wantsMultipart(op.headers, resolvedFields);
            auto formBody = buildFormBody(resolvedFields, routeMultipart);
            if (!formBody) {
                result.status = StepResult::Status::Failed;
                result.error = formBody.error().code;
                result.detail = formBody.error().detail;
                auto elapsed = std::chrono::steady_clock::now() - startTime;
                result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
                return result;
            }
            if (auto* mp = std::get_if<MultipartBody>(&*formBody)) {
                req.multipart = std::move(mp->parts);
                // libcurl writes Content-Type with the boundary; drop any
                // user-supplied value so curl doesn't send two headers.
                req.headers.erase("Content-Type");
            } else if (auto* enc = std::get_if<UrlEncodedBody>(&*formBody)) {
                req.body = std::move(enc->body);
                req.headers["Content-Type"] = "application/x-www-form-urlencoded";
            }
        }

        if (op.timeout) {
            req.timeout = *op.timeout;
        }

        // pre_request hook: runs after the request is fully built but
        // before any signing or send. Hooks may mutate url/headers/body.
        // Method is locked once the operation type-selects it; signing
        // strategies that depend on method see the post-hook value.
        if (op.preRequestScript && deps.hooks) {
            auto hctx = buildHookContext(req, ctx, rctx, project);
            auto outcome = deps.hooks->runPreRequest(*op.preRequestScript, std::move(hctx));
            if (!outcome) {
                result.status = StepResult::Status::Failed;
                result.error = outcome.error().code;
                result.detail = outcome.error().detail;
                auto elapsed = std::chrono::steady_clock::now() - startTime;
                result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
                return result;
            }
            req.url = std::move(outcome->mutatedRequest.url);
            req.headers = std::move(outcome->mutatedRequest.headers);
            req.body = std::move(outcome->mutatedRequest.body);
        }

        const int maxAttempts = op.retry.maxAttempts;
        std::optional<HttpResponse> httpResp;
        ChainApiError lastError{};
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
                                      std::chrono::system_clock::now()});
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

                    // Rebuild the actor-injected headers with the new
                    // session. Op-declared headers are re-resolved too —
                    // `Authorization: Bearer {{user.token}}` referenced
                    // the OLD token in the first attempt's req.
                    for (const auto& [k, v] : op.headers) {
                        req.headers[k] = varResolver.resolve(v, ctx, rctx).output;
                    }
                    for (const auto& [k, v] : actorIt->second.inject.headers) {
                        req.headers[k] = varResolver.resolve(v, ctx, rctx).output;
                    }
                    if (const auto* session = ctx.session(op.actor); session) {
                        for (const auto& [k, v] : session->injectHeaders) {
                            req.headers[k] = v;
                        }
                    }

                    // Refresh the Cookie header for the retry. Mirror
                    if (!req.headers.contains("Cookie")) {
                        if (const auto jar = ctx.cookies(op.actor); !jar.empty()) {
                            req.headers["Cookie"] = cookies::formatRequestHeader(jar);
                        }
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
                                              std::chrono::system_clock::now()});
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
            std::string bodyExcerpt = httpResp->body.size() > kBodyExcerpt
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
            auto detailed = extractFromResponseDetailed(
                op.id, httpResp->body, httpResp->status, httpResp->headers, op.extractions);
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

            if (!detailed->values.empty()) {
                ResourceInstance instance;
                instance.variables = std::move(detailed->values);

                // Names only — the per-extraction ExtractionCompleted
                // events above carry the values (masked when sensitive).
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

std::expected<RunResult, ChainApiError> ExecutionEngine::run(const Project& project,
                                                             const OperationId& target,
                                                             RunContext& ctx,
                                                             const RunOptions& options) {
    impl_->cancelledRunId.store(0, std::memory_order_release);

    if (options.resetExtractions) {
        ctx.clearExtractions();
    }
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
                    result.steps.push_back(skipResult);
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
            result.steps.push_back(dryResult);
            continue;
        }

        impl_->emit(StepStarted{runId, i, opId, 1, std::chrono::system_clock::now()});

        std::vector<StepResult> pollAttemptRows;
        auto stepResult = impl_->executeStep(op, project, ctx, rctx, runId, i, pollAttemptRows);
        // Per-attempt rows precede the parent step row so renderers can
        // group them under the operation that owned the poll loop.
        for (auto& row : pollAttemptRows) {
            result.steps.push_back(std::move(row));
        }
        result.steps.push_back(stepResult);
        ctx.record(stepResult);

        if (stepResult.status == StepResult::Status::Failed) {
            impl_->emit(StepFailed{runId,
                                   i,
                                   opId,
                                   stepResult.error.value_or(ErrorCode::Http4xx),
                                   classify(stepResult.error.value_or(ErrorCode::Http4xx)),
                                   stepResult.attempts,
                                   stepResult.detail,
                                   std::chrono::system_clock::now()});
            result.outcome = RunOutcome::Failed;
            for (std::size_t j = i + 1; j < chain.size(); ++j) {
                StepResult blocked;
                blocked.op = chain[j];
                blocked.status = StepResult::Status::Blocked;
                result.steps.push_back(blocked);
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
                result.steps.push_back(cancelled);
                impl_->emit(StepCancelled{runId, j, chain[j], std::chrono::system_clock::now()});
            }
            break;
        }
    }

    impl_->emit(RunEnded{
        runId, result.outcome, std::chrono::milliseconds{0}, std::chrono::system_clock::now()});
    return result;
}

void ExecutionEngine::cancel(RunId run) {
    impl_->cancelledRunId.store(run.value, std::memory_order_release);
}

void ExecutionEngine::subscribe(EventCallback callback) {
    const std::lock_guard lock(impl_->subscriberMutex);
    impl_->subscribers.push_back(std::move(callback));
}

}  // namespace chainapi::engine
