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
#include "JsonExtraction.h"
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

    void emit(const RunEvent& e) {
        // Snapshot before invoking — avoids re-entrant deadlock if a callback calls subscribe().
        std::vector<EventCallback> snapshot;
        {
            const std::lock_guard lock(subscriberMutex);
            snapshot = subscribers;
        }
        for (auto& cb : snapshot) {
            try {
                cb(e);
            } catch (...) {  // never let a subscriber break the engine
            }
        }
    }

    // Authenticate an actor if session is not live. Returns true on success.
    bool ensureSession(const Actor& actor,
                       RunContext& ctx,
                       const ResolveContext& rctx,
                       RunId /*runId*/) {
        auto existing = ctx.session(actor.id);
        if (existing && existing->state == ActorSession::State::Live) {
            const auto now = std::chrono::steady_clock::now();
            if (now < existing->expiresAt) {
                return true;
            }
        }

        auto authenticator =
            selectAuthenticator(actor, AuthDependencies{deps.http.get(), &varResolver});
        if (!authenticator) return false;

        auto outcome = authenticator->authenticate(actor, ctx, rctx);
        if (!outcome) return false;

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
            if (it != project.actors.end()) pollActor = &it->second;
        } else if (!op.actor.value.empty()) {
            auto it = project.actors.find(op.actor);
            if (it != project.actors.end()) pollActor = &it->second;
        }

        if (pollActor && !ensureSession(*pollActor, ctx, rctx, runId)) {
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
            auto resolvedPath = varResolver.resolve(poll.pathTemplate, ctx, rctx);
            if (!resolvedPath.unresolved.empty()) {
                return std::unexpected(ChainApiError{
                    ErrorCode::VarUnresolved,
                    ErrorClass::Resolution,
                    "poll_until: unresolved variable in path: " + resolvedPath.unresolved.front()});
            }
            req.url = baseUrl + resolvedPath.output;
            if (pollActor) {
                for (const auto& [k, v] : pollActor->inject.headers) {
                    auto resolved = varResolver.resolve(v, ctx, rctx);
                    req.headers[k] = resolved.output;
                }
                // Session-level inject. Session wins on key collision.
                if (auto* session = ctx.session(pollActor->id); session) {
                    for (const auto& [k, v] : session->injectHeaders) {
                        req.headers[k] = v;
                    }
                    if (!session->injectQueryParams.empty()) {
                        std::string qs;
                        for (const auto& [k, v] : session->injectQueryParams) {
                            if (!qs.empty()) qs += "&";
                            qs += urlEncode(k) + "=" + urlEncode(v);
                        }
                        req.url += (req.url.find('?') == std::string::npos ? "?" : "&") + qs;
                    }
                }
            }

            // Per-request signing done after inject merge so the signer sees the final shape.
            if (pollActor) {
                if (auto* session = ctx.session(pollActor->id); session) {
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

            auto resp = deps.http->send(req);
            if (!resp) {
                return std::unexpected(resp.error());
            }
            lastResponse = std::move(*resp);
            haveLastResponse = true;

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
            if (delay < kMinPollDelay) delay = kMinPollDelay;

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
                if (!ensureSession(actorIt->second, ctx, rctx, runId)) {
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
                if (auto* session = ctx.session(op.actor); session) {
                    for (const auto& [k, v] : session->injectHeaders) {
                        req.headers[k] = v;
                    }
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
            if (auto* session = ctx.session(op.actor); session) {
                for (const auto& [k, v] : session->injectQueryParams) {
                    queryParams[k] = v;
                }
            }
        }
        if (!queryParams.empty()) {
            std::string qs;
            for (const auto& [k, v] : queryParams) {
                if (!qs.empty()) qs += "&";
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
            std::string formBody;
            for (const auto& [k, v] : *op.bodyForm) {
                auto resolved = varResolver.resolve(v, ctx, rctx);
                if (!formBody.empty()) formBody += "&";
                formBody += urlEncode(k) + "=" + urlEncode(resolved.output);
            }
            req.body = formBody;
            req.headers["Content-Type"] = "application/x-www-form-urlencoded";
        }

        if (op.timeout) {
            req.timeout = *op.timeout;
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
                if (auto* session = ctx.session(op.actor); session) {
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

            auto resp = deps.http->send(req);
            if (resp) {
                httpResp = std::move(*resp);
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
            if (delay > op.retry.maxBackoff) delay = op.retry.maxBackoff;
            std::this_thread::sleep_for(delay);
        }

        result.attempts = attemptCount;

        // When expectStatusList is non-empty it takes precedence over the
        // singular expectStatus field; the latter is the legacy single-value
        // form consulted only when the list is empty.
        const auto statusMatches = [&]() -> bool {
            if (!httpResp) return true;
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
            const auto pollResult = runPollLoop(
                op, *op.pollUntil, project, ctx, rctx, runId, *httpResp, pollAttemptRows);
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

        if (httpResp && !op.extractions.empty()) {
            auto detailed = extractFromJsonDetailed(op.id, httpResp->body, op.extractions);
            if (!detailed) {
                result.status = StepResult::Status::Failed;
                result.error = detailed.error().code;
                auto elapsed = std::chrono::steady_clock::now() - startTime;
                result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
                return result;
            }

            // Trace every extraction outcome — including misses — so the
            // timeline shows nulls and missing fields. The op still fails
            // when any required extraction misses; that contract pre-dates
            // this slice and downstream ops depend on it.
            std::optional<std::string> firstMiss;
            for (auto& t : detailed->traces) {
                if (!firstMiss && t.outcome == ExtractionTrace::Outcome::Missing) {
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
                        ev.outcome = ExtractionCompleted::Outcome::Resolved;
                        ev.value = t.value;
                        break;
                    case ExtractionTrace::Outcome::Null:
                        ev.outcome = ExtractionCompleted::Outcome::Null;
                        break;
                    case ExtractionTrace::Outcome::Missing:
                        ev.outcome = ExtractionCompleted::Outcome::Missing;
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

    impl_->emit(RunStarted{runId, target, chain.size(), envName, std::chrono::system_clock::now()});

    RunResult result;
    result.runId = runId;
    result.outcome = RunOutcome::Succeeded;

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
            result.outcome = RunOutcome::Cancelled;
            for (std::size_t j = i + 1; j < chain.size(); ++j) {
                StepResult cancelled;
                cancelled.op = chain[j];
                cancelled.status = StepResult::Status::Cancelled;
                result.steps.push_back(cancelled);
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
