// ExecutionEngine 
//
// Resolves the dependency chain, authenticates actors as needed, executes
// each step with variable substitution, extracts response values, and
// caches sessions/extractions for reuse.
#include <chainapi/engine/ExecutionEngine.h>

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

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
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

/// RFC 3986 unreserved characters pass through; everything else is %-encoded.
std::string urlEncode(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (char rawChar : in) {
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

}  // namespace

struct ExecutionEngine::Impl {
    Dependencies deps;
    DependencyResolver resolver;
    VariableResolver varResolver;
    std::vector<EventCallback> subscribers;
    std::mutex subscriberMutex;
    std::atomic<std::uint64_t> nextRunId{1};
    /// 0 = nothing cancelled. Any other value = the run with that id is being cancelled.
    /// Set via cancel(RunId), read by the executing run loop.
    std::atomic<std::uint64_t> cancelledRunId{0};

    explicit Impl(Dependencies d) : deps(std::move(d)) {}

    [[nodiscard]] bool isCancelled(RunId runId) const noexcept {
        const auto cancelled = cancelledRunId.load(std::memory_order_acquire);
        return cancelled != 0 && cancelled == runId.value;
    }

    void emit(const RunEvent& e) {
        // Snapshot subscribers, then invoke without holding the lock — avoids
        // re-entrant deadlock if a callback calls subscribe(), and avoids
        // exception propagation through the engine's control flow.
        std::vector<EventCallback> snapshot;
        {
            const std::lock_guard lock(subscriberMutex);
            snapshot = subscribers;
        }
        for (auto& cb : snapshot) {
            try { cb(e); } catch (...) { /* never let a subscriber break the engine */ }
        }
    }

    /// Authenticate an actor if session is not live. Returns true on success.
    ///
    ///  the strategy-specific work to an Authenticator
    /// (`engine/src/application/AuthStrategy.h`); this method owns the
    /// session-cache check, the post-success state flip, and the
    /// expiry-deadline computation.
    bool ensureSession(const Actor& actor, RunContext& ctx,
                       const ResolveContext& rctx, RunId /*runId*/) {
        auto existing = ctx.session(actor.id);
        if (existing && existing->state == ActorSession::State::Live) {
            const auto now = std::chrono::steady_clock::now();
            if (now < existing->expiresAt) {
                return true;  // Cache hit
            }
        }

        auto authenticator = selectAuthenticator(
            actor, AuthDependencies{deps.http.get(), &varResolver});
        if (!authenticator) return false;

        auto outcome = authenticator->authenticate(actor, ctx, rctx);
        if (!outcome) return false;

        ActorSession session = std::move(*outcome);
        session.state = ActorSession::State::Live;
        session.expiresAt =
            std::chrono::steady_clock::now() + actor.sessionTtl;
        ctx.putSession(actor.id, std::move(session));
        return true;
    }

    /// Run the polling phase for an operation.
    ///
    /// Returns the FINAL poll response on success — the caller substitutes
    /// it for the initial response so extractions run against the completion
    /// payload. Returns ChainApiError with a Poll* code on:
    ///   - PollFailPredicate: fail_when matched a response
    ///   - PollTimeout:        wall-clock budget exceeded
    ///   - PollMaxAttemptsExceeded: attempt-count budget exceeded
    ///   - SchemaInvalid:      success_when / fail_when failed to parse
    ///                          (caught at run time only — schema-time
    ///                          parsing is a Slice 3d concern)
    ///
    /// Cancellation: the loop checks isCancelled() each iteration and
    /// returns Cancelled cleanly so the parent step can mark itself
    /// Cancelled instead of Failed.
    std::expected<HttpResponse, ChainApiError>
    runPollLoop(const Operation& op,
                const PollUntil& poll,
                const Project& project,
                RunContext& ctx,
                const ResolveContext& rctx,
                RunId runId,
                const HttpResponse& /*initialResponse*/) {
        PredicateEvaluator evaluator;

        auto successPredicate = evaluator.parse(poll.successWhen);
        if (!successPredicate) {
            return std::unexpected(ChainApiError{
                ErrorCode::SchemaInvalid,
                ErrorClass::Schema,
                "poll_until.success_when: " + successPredicate.error().detail});
        }

        std::optional<ParsedPredicate> failPredicate;
        if (poll.failWhen) {
            auto parsed = evaluator.parse(*poll.failWhen);
            if (!parsed) {
                return std::unexpected(ChainApiError{
                    ErrorCode::SchemaInvalid,
                    ErrorClass::Schema,
                    "poll_until.fail_when: " + parsed.error().detail});
            }
            failPredicate = std::move(*parsed);
        }

        // Resolve the polling URL once per attempt — the response from
        // attempt N may influence attempt N+1's URL (rare, but allowed).
        const auto baseUrlIt = rctx.envVars.find("baseUrl");
        const std::string baseUrl =
            baseUrlIt != rctx.envVars.end() ? baseUrlIt->second : "";

        // Determine which actor's inject headers to use.
        const Actor* pollActor = nullptr;
        if (poll.actor) {
            auto it = project.actors.find(*poll.actor);
            if (it != project.actors.end()) pollActor = &it->second;
        } else if (!op.actor.value.empty()) {
            auto it = project.actors.find(op.actor);
            if (it != project.actors.end()) pollActor = &it->second;
        }

        // Ensure the polling actor has a live session.
        if (pollActor && !ensureSession(*pollActor, ctx, rctx, runId)) {
            return std::unexpected(ChainApiError{
                ErrorCode::SessionRefreshFailed, ErrorClass::Auth,
                "poll_until: actor session refresh failed"});
        }

        const auto deadline = std::chrono::steady_clock::now() + poll.timeout;
        HttpResponse lastResponse;
        bool haveLastResponse = false;

        for (int attempt = 0; attempt < poll.maxAttempts; ++attempt) {
            if (isCancelled(runId)) {
                return std::unexpected(ChainApiError{
                    ErrorCode::Cancelled, ErrorClass::Run,
                    "poll_until: cancelled"});
            }

            HttpRequest req;
            req.method = poll.method;
            auto resolvedPath = varResolver.resolve(poll.pathTemplate, ctx, rctx);
            if (!resolvedPath.unresolved.empty()) {
                return std::unexpected(ChainApiError{
                    ErrorCode::VarUnresolved, ErrorClass::Resolution,
                    "poll_until: unresolved variable in path: " +
                    resolvedPath.unresolved.front()});
            }
            req.url = baseUrl + resolvedPath.output;
            if (pollActor) {
                for (const auto& [k, v] : pollActor->inject.headers) {
                    auto resolved = varResolver.resolve(v, ctx, rctx);
                    req.headers[k] = resolved.output;
                }
                // Session-level inject . Same precedence
                // rule as the executor: session wins on collision.
                if (auto* session = ctx.session(pollActor->id); session) {
                    for (const auto& [k, v] : session->injectHeaders) {
                        req.headers[k] = v;
                    }
                    // The polling URL is built from `pathTemplate`
                    // alone today; query-param injects on the parent
                    // actor would silently miss the poll endpoint.
                    // Append them explicitly so api_key + query-form
                    // auth carries through to the status fetch.
                    if (!session->injectQueryParams.empty()) {
                        std::string qs;
                        for (const auto& [k, v] : session->injectQueryParams) {
                            if (!qs.empty()) qs += "&";
                            qs += urlEncode(k) + "=" + urlEncode(v);
                        }
                        req.url += (req.url.find('?') == std::string::npos
                                        ? "?"
                                        : "&") + qs;
                    }
                }
            }

            auto resp = deps.http->send(req);
            if (!resp) {
                // Network error during polling — treat the same as the
                // initial-request retry policy: bail with the network code.
                return std::unexpected(resp.error());
            }
            lastResponse = std::move(*resp);
            haveLastResponse = true;

            // fail_when wins over success_when when both match.
            if (failPredicate &&
                evaluator.evaluate(*failPredicate, lastResponse.body,
                                   lastResponse.status) ==
                    PredicateValue::True) {
                return std::unexpected(ChainApiError{
                    ErrorCode::PollFailPredicate, ErrorClass::Polling,
                    "poll_until.fail_when matched (HTTP " +
                    std::to_string(lastResponse.status) + ")"});
            }
            if (evaluator.evaluate(*successPredicate, lastResponse.body,
                                   lastResponse.status) ==
                    PredicateValue::True) {
                return lastResponse;
            }

            // Compute next-attempt delay. Either fixed interval or
            // exponential backoff with jitter — never both.
            std::chrono::milliseconds delay = poll.interval;
            if (poll.backoffBase) {
                const auto shift = std::min(attempt, 20);
                auto raw = *poll.backoffBase * (std::uint32_t{1} << shift);
                delay = (raw < poll.backoffMax) ? raw : poll.backoffMax;
            }

            // Floor the inter-poll delay at a small but non-zero minimum
            // so a misconfigured poll (`interval: 0ms`, no backoff) does
            // not busy-loop the SUT until maxAttempts. 
            constexpr auto kMinPollDelay = std::chrono::milliseconds{50};
            if (delay < kMinPollDelay) delay = kMinPollDelay;

            // Honour the wall-clock deadline: never sleep past it.
            const auto remaining = deadline - std::chrono::steady_clock::now();
            if (remaining <= std::chrono::milliseconds{0}) {
                return std::unexpected(ChainApiError{
                    ErrorCode::PollTimeout, ErrorClass::Polling,
                    haveLastResponse
                        ? "poll_until: timeout exceeded — last response: HTTP " +
                          std::to_string(lastResponse.status)
                        : "poll_until: timeout exceeded"});
            }
            const auto sleepFor = std::min(
                std::chrono::duration_cast<std::chrono::milliseconds>(remaining),
                delay);
            std::this_thread::sleep_for(sleepFor);
        }

        return std::unexpected(ChainApiError{
            ErrorCode::PollMaxAttemptsExceeded, ErrorClass::Polling,
            haveLastResponse
                ? "poll_until: max_attempts (" +
                  std::to_string(poll.maxAttempts) +
                  ") exceeded — last response: HTTP " +
                  std::to_string(lastResponse.status)
                : "poll_until: max_attempts (" +
                  std::to_string(poll.maxAttempts) + ") exceeded"});
    }

    /// Execute a single operation step. Returns the StepResult.
    StepResult executeStep(const Operation& op, const Project& project,
                           RunContext& ctx, const ResolveContext& rctx,
                           RunId runId, std::size_t /*stepIndex*/) {
        StepResult result;
        result.op = op.id;
        result.attempts = 1;
        auto startTime = std::chrono::steady_clock::now();

        // Ensure actor session if needed.
        if (!op.actor.value.empty()) {
            auto actorIt = project.actors.find(op.actor);
            if (actorIt != project.actors.end()) {
                // Apply actor inject headers later.
                if (!ensureSession(actorIt->second, ctx, rctx, runId)) {
                    result.status = StepResult::Status::Failed;
                    result.error = ErrorCode::SessionRefreshFailed;
                    return result;
                }
            }
        }

        // Resolve the request.
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

        // Operation headers.
        for (const auto& [k, v] : op.headers) {
            auto resolved = varResolver.resolve(v, ctx, rctx);
            req.headers[k] = resolved.output;
        }

        // Actor inject headers.
        if (!op.actor.value.empty()) {
            auto actorIt = project.actors.find(op.actor);
            if (actorIt != project.actors.end()) {
                for (const auto& [k, v] : actorIt->second.inject.headers) {
                    auto resolved = varResolver.resolve(v, ctx, rctx);
                    req.headers[k] = resolved.output;
                }
                // Session-level inject .
                // Strategies like api_key auto-populate these; we apply
                // them after the static inject block so the strategy's
                // resolved values win on key collision. Already-resolved
                // — no `varResolver.resolve` call needed.
                if (auto* session = ctx.session(op.actor); session) {
                    for (const auto& [k, v] : session->injectHeaders) {
                        req.headers[k] = v;
                    }
                }
            }
        }

        // Query params → append to URL.
        // Session-level injects  are folded in
        // alongside the operation's static query params; session values
        // win on key collision, mirroring the header-inject precedence.
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

        // Body.
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

        // Send with retry. Track real attempt count for the result.
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
            // Exponential backoff with bounded shift to avoid signed-overflow UB
            // on large maxAttempts values. Cap the shift at 20 (≈ 1M ms multiplier).
            const auto shift = std::min(attempt, 20);
            auto delay = op.retry.baseBackoff * (std::uint32_t{1} << shift);
            if (delay > op.retry.maxBackoff) delay = op.retry.maxBackoff;
            std::this_thread::sleep_for(delay);
        }

        result.attempts = attemptCount;

        // Status check. when expectStatusList is non-empty
        // (e.g. `expect_status: [200, 202]`) it takes precedence over
        // the singular expectStatus field; the latter is the legacy
        // single-value form and only consulted when the list is empty.
        const auto statusMatches = [&]() -> bool {
            if (!httpResp) return true;  // network failure already handled
            if (!op.expectStatusList.empty()) {
                return std::find(op.expectStatusList.begin(),
                                 op.expectStatusList.end(),
                                 httpResp->status) != op.expectStatusList.end();
            }
            if (op.expectStatus) {
                return httpResp->status == *op.expectStatus;
            }
            return true;  // no expectation declared
        }();

        if (!statusMatches) {
            result.status = StepResult::Status::Failed;
            result.error = (httpResp->status >= 500) ? ErrorCode::Http5xx : ErrorCode::Http4xx;
            // Capture HTTP status + a short body excerpt so the user can
            // see exactly what the server said.
            constexpr std::size_t kBodyExcerpt = 200;
            std::string bodyExcerpt = httpResp->body.size() > kBodyExcerpt
                ? httpResp->body.substr(0, kBodyExcerpt) + "..."
                : httpResp->body;
            result.detail = "HTTP " + std::to_string(httpResp->status)
                + " — " + bodyExcerpt;
            auto elapsed = std::chrono::steady_clock::now() - startTime;
            result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
            return result;
        }

        // ── Polling phase ────────────────────────────────
        // When the operation declares poll_until, the initial response
        // is treated as a launch acknowledgement; the engine polls a
        // status endpoint until success_when matches (then extractions
        // run against the FINAL poll response) or fail_when matches
        // (the step fails with PollFailPredicate) or one of the budgets
        // fires (PollTimeout / PollMaxAttemptsExceeded).
        if (op.pollUntil && httpResp) {
            const auto pollResult = runPollLoop(op, *op.pollUntil, project,
                                                ctx, rctx, runId, *httpResp);
            if (!pollResult.has_value()) {
                result.status = StepResult::Status::Failed;
                result.error = pollResult.error().code;
                result.detail = pollResult.error().detail;
                auto elapsed = std::chrono::steady_clock::now() - startTime;
                result.elapsed = std::chrono::duration_cast<
                    std::chrono::milliseconds>(elapsed);
                return result;
            }
            // Replace the response we extract from with the final poll
            // response. Subsequent extraction logic is unchanged.
            httpResp = std::move(*pollResult);
        }

        // Extract variables — surfaces ResponseParse / ExtractionFailed
        if (httpResp && !op.extractions.empty()) {
            auto values = extractFromJson(httpResp->body, op.extractions);
            if (!values) {
                result.status = StepResult::Status::Failed;
                result.error = values.error().code;
                auto elapsed = std::chrono::steady_clock::now() - startTime;
                result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
                return result;
            }
            if (!values->empty()) {
                ResourceInstance instance;
                instance.variables = std::move(*values);
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

std::expected<RunResult, ChainApiError> ExecutionEngine::run(
    const Project& project,
    const OperationId& target,
    RunContext& ctx,
    const RunOptions& options) {

    impl_->cancelledRunId.store(0, std::memory_order_release);

    // Handle reset options.
    if (options.resetExtractions) {
        ctx.clearExtractions();
    }
    if (options.resetSessions) {
        // "Send Cleanly" — clear sessions only. Extractions are independent

        for (const auto& [actorId, _] : project.actors) {
            ctx.invalidateSession(actorId);
        }
    }

    // Resolve the chain.
    auto chainResult = impl_->resolver.resolve(project, target);
    if (!chainResult) {
        return std::unexpected(chainResult.error());
    }

    const auto& chain = *chainResult;
    auto runId = RunId{impl_->nextRunId.fetch_add(1)};

    // Build resolve context from environment.
    ResolveContext rctx;
    auto envName = options.environment.empty() ? project.defaultEnvironment : options.environment;
    if (project.environments.contains(envName)) {
        rctx.envVars = project.environments.at(envName);
    }

    // Emit RunStarted.
    impl_->emit(RunStarted{runId, target, chain.size(), envName,
                           std::chrono::system_clock::now()});

    RunResult result;
    result.runId = runId;
    result.outcome = RunOutcome::Succeeded;

    // Execute each step.
    for (std::size_t i = 0; i < chain.size(); ++i) {
        const auto& opId = chain[i];

        // Find the operation.
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
                // Check if all required extractions are already present.
                bool allPresent = true;
                for (const auto& ext : op.extractions) {
                    bool found = false;
                    for (const auto& inst : instances) {
                        if (inst.variables.contains(ext.variableName)) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) { allPresent = false; break; }
                }
                if (allPresent) {
                    StepResult skipResult;
                    skipResult.op = opId;
                    skipResult.status = StepResult::Status::Skipped;
                    result.steps.push_back(skipResult);
                    impl_->emit(StepSkipped{runId, i, opId,
                                            SkipReason::ExtractionCached,
                                            std::chrono::system_clock::now()});
                    continue;
                }
            }
        }

        // Dry run: don't execute.
        if (options.dryRun) {
            StepResult dryResult;
            dryResult.op = opId;
            dryResult.status = StepResult::Status::Succeeded;
            result.steps.push_back(dryResult);
            continue;
        }

        impl_->emit(StepStarted{runId, i, opId, 1,
                                std::chrono::system_clock::now()});

        auto stepResult = impl_->executeStep(op, project, ctx, rctx, runId, i);
        result.steps.push_back(stepResult);
        ctx.record(stepResult);

        if (stepResult.status == StepResult::Status::Failed) {
            impl_->emit(StepFailed{runId, i, opId,
                                   stepResult.error.value_or(ErrorCode::Http4xx),
                                   classify(stepResult.error.value_or(ErrorCode::Http4xx)),
                                   stepResult.attempts, stepResult.detail,
                                   std::chrono::system_clock::now()});
            result.outcome = RunOutcome::Failed;
            // Mark remaining as blocked.
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

    impl_->emit(RunEnded{runId, result.outcome, std::chrono::milliseconds{0},
                         std::chrono::system_clock::now()});
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
