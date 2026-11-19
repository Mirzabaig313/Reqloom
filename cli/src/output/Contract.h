// The machine contract every `reqloom` JSON document carries (PRD FR-13.11).
#pragma once

#include <string_view>

namespace reqloom::cli {

/// Version of the document shapes, written as the top-level `schema_version`; it changes only
/// when a field is renamed or removed, which the contract forbids within a major version.
inline constexpr int kSchemaVersion = 1;

/// Version of the agent contract, `<schema_version>.<minor>`, written as `contract_version`. The
/// minor part grows with each additive change:
///   1.1  run: the response, extracted values, `next_action`, `failed_step`
///   1.2  list, describe, plan and show documents; lint gains `contract_version`, positions,
///        `reference`, `fix` and `next_action` on errors, and `warning_details`; run gains
///        `recorded` (its masked steps are in run history for `show`)
inline constexpr std::string_view kContractVersion = "1.2";

}  // namespace reqloom::cli
