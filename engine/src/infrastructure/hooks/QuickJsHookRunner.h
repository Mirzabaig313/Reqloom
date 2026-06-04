#pragma once

#include "HookRunner.h"

namespace reqloom::engine {

class QuickJsHookRunner final : public HookRunner {
public:
    QuickJsHookRunner();
    QuickJsHookRunner(const QuickJsHookRunner&) = delete;
    QuickJsHookRunner& operator=(const QuickJsHookRunner&) = delete;
    QuickJsHookRunner(QuickJsHookRunner&&) = delete;
    QuickJsHookRunner& operator=(QuickJsHookRunner&&) = delete;
    ~QuickJsHookRunner() override;

    std::expected<HookOutcome, ReqloomError> runPreRequest(const std::string& script,
                                                            HookContext context) override;

    std::expected<HookOutcome, ReqloomError> runPostResponse(const std::string& script,
                                                              HookContext context) override;
};

}  // namespace reqloom::engine
