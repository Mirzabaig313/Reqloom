#pragma once

#include <chainapi/engine/ExecutionEngine.h>

#include <expected>

namespace chainapi::engine {

/// Application-layer use case wrapping ExecutionEngine. Kept as a separate
/// abstraction so the CLI and UI share the same conceptual entry point as
/// the engine evolves.
class RunOperationUseCase {
public:
    explicit RunOperationUseCase(ExecutionEngine& engine);

    [[nodiscard]] std::expected<RunResult, ChainApiError> execute(const Project& project,
                                                                  const OperationId& target,
                                                                  RunContext& ctx,
                                                                  const RunOptions& options = {});

private:
    ExecutionEngine& engine_;
};

}  // namespace chainapi::engine
