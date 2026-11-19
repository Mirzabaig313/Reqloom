// Machine-readable JSON renderer for `reqloom run --format json`.

#pragma once

#include "Contract.h"

#include <reqloom/engine/PublicApi.h>

#include <optional>
#include <ostream>
#include <string_view>

namespace reqloom::cli {

/// Emits a single JSON document describing the run. The schema is stable for CI consumers;
/// `schema_version` changes when a field is renamed or removed.
class JsonRenderer {
public:
    /// Version of the document shape, written as the top-level `schema_version` (Contract.h).
    static constexpr int kSchemaVersion = ::reqloom::cli::kSchemaVersion;

    /// Version of the agent contract shared by every command's JSON (Contract.h).
    static constexpr std::string_view kContractVersion = ::reqloom::cli::kContractVersion;

    explicit JsonRenderer(std::ostream& out);

    /// `recorded`: the run's masked step rows are in run history, so `reqloom show <run_id>
    /// <index>` can print one (written as the top-level `recorded`).
    void render(const engine::OperationId& target,
                std::string_view environment,
                const engine::RunResult& result,
                bool recorded = false);

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
