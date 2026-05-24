#include "LintCommand.h"

#include <print>

namespace chainapi::cli {

int lintCommand(const QStringList& /*args*/) {
    // Phase 1: parse project, run schema validation, report cycles /
    // undefined references with file:line.
    std::println("[stub] lint not yet implemented");
    return 0;
}

}  // namespace chainapi::cli
