// Engine-internal interface for executing pre/post hooks in a sandboxed
// JS environment. Concrete impl: QuickJsHookRunner.
//
// Sandbox rules:
//   - No filesystem access
//   - No network beyond the operation's own request
//   - 1-second timeout per hook
//   - Read-only access to other actors' variables
#pragma once

#include <chainapi/engine/ErrorCodes.h>
#include <chainapi/engine/Operation.h>

#include <expected>
#include <map>
#include <optional>
#include <string>

namespace chainapi::engine {

struct HookRequestView {
    HttpMethod method{};
    std::string url;
    std::map<std::string, std::string> headers;
    std::optional<std::string> body;
};

struct HookResponseView {
    int status{0};
    std::map<std::string, std::string> headers;
    std::string body;
};

struct HookContext {
    HookRequestView request;
    std::optional<HookResponseView> response;  ///< Set only for post_response.
    std::map<std::string, std::map<std::string, std::string>> variables;
    std::map<std::string, std::string> env;
    std::map<std::string, std::string> secrets;
};

struct HookOutcome {
    HookRequestView mutatedRequest;                   ///< pre_request hooks may mutate.
    std::optional<HookResponseView> mutatedResponse;  ///< post_response hooks may mutate.
};

class HookRunner {
public:
    virtual ~HookRunner() = default;

    virtual std::expected<HookOutcome, ChainApiError> runPreRequest(const std::string& script,
                                                                    HookContext context) = 0;

    virtual std::expected<HookOutcome, ChainApiError> runPostResponse(const std::string& script,
                                                                      HookContext context) = 0;
};

}  // namespace chainapi::engine
