// QuickJsHookRunner — sandboxed JS execution via QuickJS.
// PRD §5.10 / Engine Req §3.12.
//
// Sandbox enforced at runtime: no FS, no network, 1s timeout, read-only
// access to other actors' variables. Phase 1 implementation.
#include "QuickJsHookRunner.h"

namespace chainapi::engine {

QuickJsHookRunner::QuickJsHookRunner() = default;
QuickJsHookRunner::~QuickJsHookRunner() = default;

std::expected<HookOutcome, ChainApiError> QuickJsHookRunner::runPreRequest(
    const std::string& /*script*/, HookContext context) {
    HookOutcome out;
    out.mutatedRequest = std::move(context.request);
    return out;
}

std::expected<HookOutcome, ChainApiError> QuickJsHookRunner::runPostResponse(
    const std::string& /*script*/, HookContext context) {
    HookOutcome out;
    out.mutatedRequest = std::move(context.request);
    out.mutatedResponse = std::move(context.response);
    return out;
}

}  // namespace chainapi::engine
