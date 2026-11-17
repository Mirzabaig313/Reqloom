// Machine-readable JSON renderer for `reqloom run --format json`.

#pragma once

#include <reqloom/engine/PublicApi.h>

#include <optional>
#include <ostream>
#include <string_view>

namespace reqloom::cli {

/// Emits a single JSON document describing the run. The schema is stable for CI consumers;
/// `schema_version` changes when a field is renamed or removed.
class JsonRenderer {
public:
    /// Version of the document shape, written as the top-level `schema_version`.
    static constexpr int kSchemaVersion = 1;

    /// Version of the agent contract, `<schema_version>.<minor>`: the minor part grows with each
    /// additive change (1.1 added the response, extracted values, `next_action`, `failed_step`).
    static constexpr std::string_view kContractVersion = "1.1";

    explicit JsonRenderer(std::ostream& out);

    void render(const engine::OperationId& target,
                std::string_view environment,
                const engine::RunResult& result);

    /// A run that ended before its first step: the same top-level shape with `outcome` "ERROR",
    /// an empty `steps` array and `error: { code, detail }`. `run` is null when no run started.
    void renderError(const engine::OperationId& target,
                     std::string_view environment,
                     std::optional<engine::RunId> run,
                     std::string_view code,
                     std::string_view detail);

private:
    std::ostream& out_;
};

}  // namespace reqloom::cli
