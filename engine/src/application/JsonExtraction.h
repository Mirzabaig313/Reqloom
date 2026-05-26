// JsonExtraction — apply a list of `Extraction`s against a JSON body.
// Shared between ExecutionEngine and AuthStrategy implementations.
//
// Supported JSONPath subset:
//   $.field
//   $.nested.field
//   $.array[0].field
//   $.field[0]
#pragma once

#include <chainapi/engine/ErrorCodes.h>
#include <chainapi/engine/Operation.h>
#include <chainapi/engine/RunContext.h>

#include <expected>
#include <map>
#include <string>
#include <vector>

namespace chainapi::engine {

[[nodiscard]] std::expected<std::map<std::string, std::string>, ChainApiError> extractFromJson(
    const std::string& body, const std::vector<Extraction>& extractions);

/// Per-extraction outcome including the resolved value (when present).
/// Unlike `extractFromJson`, this never short-circuits — every entry in
/// `extractions` produces one trace row, so callers that want to record
/// the full extraction history (the executor, for the timeline UI) can.
///
/// Returns `ResponseParse` only when the body is not valid JSON. A path
/// that doesn't resolve is NOT an error here — it surfaces as
/// `ExtractionTrace::Outcome::Missing`.
struct DetailedExtraction {
    std::vector<ExtractionTrace> traces;
    std::map<std::string, std::string> values;  ///< Resolved values only.
};

[[nodiscard]] std::expected<DetailedExtraction, ChainApiError> extractFromJsonDetailed(
    const OperationId& opId, const std::string& body, const std::vector<Extraction>& extractions);

}  // namespace chainapi::engine
