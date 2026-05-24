#include "RunOperationUseCase.h"

namespace chainapi::engine {

RunOperationUseCase::RunOperationUseCase(ExecutionEngine& engine)
    : engine_(engine) {}

std::expected<RunResult, ChainApiError> RunOperationUseCase::execute(
    const Project& project,
    const OperationId& target,
    RunContext& ctx,
    const RunOptions& options) {
    return engine_.run(project, target, ctx, options);
}

}  // namespace chainapi::engine
