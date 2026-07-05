// Public, stateless hook dry-run / validation.
//
// Runs a pre-request or post-response hook script in the same sandboxed
// JS environment the engine uses at run time (no filesystem, no network,
// 1-second budget), against a caller-supplied sample context. Lets an editor
// validate a hook and preview its mutations without executing a real request.
#pragma once

#include <reqloom/engine/ErrorCodes.h>
#include <reqloom/engine/Operation.h>

#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>

namespace reqloom::engine {

/// Which lifecycle hook to run.
enum class HookPhase : std::uint8_t {
    PreRequest,    ///< Runs before the request is sent; may mutate the request.
    PostResponse,  ///< Runs after the response arrives; may mutate the response.
};

/// The request a hook sees (and, for pre_request, may mutate).
struct HookSampleRequest {
    HttpMethod method{HttpMethod::Get};
    std::string url;
    std::map<std::string, std::string> headers;
    std::optional<std::string> body;
};

/// The response a post_response hook sees (and may mutate).
struct HookSampleResponse {
    int status{0};
    std::map<std::string, std::string> headers;
    std::string body;
};

/// Inputs for a hook dry-run. `response` is required for `PostResponse` and
/// ignored for `PreRequest`. `variables`/`env`/`secrets` populate the hook's
/// read-only `ctx` surface exactly as at run time.
struct HookDryRunInput {
    HookPhase phase{HookPhase::PreRequest};
    std::string script;
    HookSampleRequest request;
    std::optional<HookSampleResponse> response;
    std::map<std::string, std::map<std::string, std::string>> variables;
    std::map<std::string, std::string> env;
    std::map<std::string, std::string> secrets;
};

/// Result of a successful dry-run: the (possibly mutated) request, plus the
/// (possibly mutated) response for a `PostResponse` run.
struct HookDryRunResult {
    HookSampleRequest request;
    std::optional<HookSampleResponse> response;
};

/// Execute `input.script` in the hook sandbox against the sample context and
/// return the mutated request/response. Returns `ReqloomError`:
///   - `HookFailure` for a syntax/runtime error (detail carries the JS error),
///     an empty script, or a `PostResponse` run with no `response` supplied;
///   - `HookTimeout` if the script exceeds the 1-second budget.
/// Pure: no filesystem, no network, no shared engine state.
[[nodiscard]] std::expected<HookDryRunResult, ReqloomError> dryRunHook(
    const HookDryRunInput& input);

}  // namespace reqloom::engine
