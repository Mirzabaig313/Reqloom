// Machine-readable JSON renderer for `reqloom run --format json`.

#pragma once

#include <reqloom/engine/PublicApi.h>

#include <ostream>
#include <string_view>

namespace reqloom::cli {

/// Emits a single JSON document describing the run: outcome, target,
/// environment, per-step status / elapsed / error code, and aggregate
/// counts. Schema is stable and intended for CI consumers.
class JsonRenderer {
public:
    explicit JsonRenderer(std::ostream& out);

    void render(const engine::OperationId& target,
                std::string_view environment,
                const engine::RunResult& result);

private:
    std::ostream& out_;
};

}  // namespace reqloom::cli
