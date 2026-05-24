// ExecutionEngine — public façade. Project Layout §3.1.
//
// Skeleton implementation. The real run loop, session lifecycle, retry
// policy, and event emission land in Phase 1 per Engine Requirement §3.x.
#include <chainapi/engine/ExecutionEngine.h>

#include "../domain/DependencyResolver.h"
#include "../infrastructure/hooks/HookRunner.h"
#include "../infrastructure/http/HttpClient.h"
#include "../infrastructure/schema/SchemaParser.h"
#include "../infrastructure/secrets/SecretStore.h"
#include "../infrastructure/storage/HistoryStore.h"

#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

namespace chainapi::engine {

struct ExecutionEngine::Impl {
    Dependencies deps;
    DependencyResolver resolver;
    std::vector<EventCallback> subscribers;
    std::mutex subscriberMutex;
    std::atomic<std::uint64_t> nextRunId{1};
    std::atomic<bool> cancelRequested{false};

    explicit Impl(Dependencies d) : deps(std::move(d)) {}

    void emit(const RunEvent& e) {
        const std::lock_guard lock(subscriberMutex);
        for (auto& cb : subscribers) {
            cb(e);
        }
    }
};

ExecutionEngine::ExecutionEngine(Dependencies deps)
    : impl_(std::make_unique<Impl>(std::move(deps))) {}

ExecutionEngine::~ExecutionEngine() = default;
ExecutionEngine::ExecutionEngine(ExecutionEngine&&) noexcept = default;
ExecutionEngine& ExecutionEngine::operator=(ExecutionEngine&&) noexcept = default;

std::expected<RunResult, ChainApiError> ExecutionEngine::run(
    const Project& project,
    const OperationId& target,
    RunContext& /*ctx*/,
    const RunOptions& /*options*/) {
    // Phase 1 will:
    //   1. Resolve chain via impl_->resolver.resolve(project, target).
    //   2. Emit RunStarted.
    //   3. For each step: prepare request, send (with retries), extract,
    //      record. Honor session cache (§3.3) and extraction cache (§3.4).
    //   4. Halt on first terminal failure (§3.6).
    //   5. Emit RunEnded.
    auto chain = impl_->resolver.resolve(project, target);
    RunResult result;
    result.runId = RunId{impl_->nextRunId.fetch_add(1)};
    result.outcome = RunOutcome::Succeeded;
    return result;
}

void ExecutionEngine::cancel(RunId /*run*/) {
    impl_->cancelRequested.store(true);
}

void ExecutionEngine::subscribe(EventCallback callback) {
    const std::lock_guard lock(impl_->subscriberMutex);
    impl_->subscribers.push_back(std::move(callback));
}

}  // namespace chainapi::engine
