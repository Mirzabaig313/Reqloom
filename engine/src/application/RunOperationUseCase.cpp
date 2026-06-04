#include "RunOperationUseCase.h"

namespace reqloom::engine {

RunOperationUseCase::RunOperationUseCase(ExecutionEngine& engine) : engine_(engine) {}

std::expected<RunResult, ReqloomError> RunOperationUseCase::execute(const Project& project,
                                                                    const OperationId& target,
                                                                    RunContext& ctx,
                                                                    const RunOptions& options) {
    return engine_.run(project, target, ctx, options);
}

}  // namespace reqloom::engine
