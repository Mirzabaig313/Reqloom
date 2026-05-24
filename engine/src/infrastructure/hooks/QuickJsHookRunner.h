#pragma once

#include "HookRunner.h"

namespace chainapi::engine {

class QuickJsHookRunner final : public HookRunner {
public:
    QuickJsHookRunner();
    ~QuickJsHookRunner() override;

    std::expected<HookOutcome, ChainApiError> runPreRequest(const std::string& script,
                                                            HookContext context) override;

    std::expected<HookOutcome, ChainApiError> runPostResponse(const std::string& script,
                                                              HookContext context) override;
};

}  // namespace chainapi::engine
