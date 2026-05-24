#include "RunCommand.h"

#include <print>

namespace chainapi::cli {

int runCommand(const QStringList& args) {
    if (args.isEmpty()) {
        std::println(stderr, "chainapi run: missing <operation>");
        return 2;
    }
    // Phase 1: load project, construct ExecutionEngine, dispatch run,
    // render via TextRenderer / JsonRenderer / JUnitRenderer.
    std::println("[stub] would run: {}", args.first().toStdString());
    return 0;
}

}  // namespace chainapi::cli
