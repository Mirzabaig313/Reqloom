#pragma once

#include <reqloom/engine/ExecutionEngine.h>

#include <expected>

namespace reqloom::engine {

/// Application-layer use case wrapping ExecutionEngine; the shared entry point for CLI and UI.
class RunOperationUseCase {
public:
    explicit RunOperationUseCase(ExecutionEngine& engine);

    [[nodiscard]] std::expected<RunResult, ReqloomError> execute(const Project& project,
                                                                 const OperationId& target,
                                                                 RunContext& ctx,
                                                                 const RunOptions& options = {});

private:
    ExecutionEngine& engine_;
};

}  // namespace reqloom::engine
