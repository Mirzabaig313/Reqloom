// JsonExtraction — apply a list of `Extraction`s against a JSON body.
// Shared between ExecutionEngine and AuthStrategy implementations.
//
// Supported JSONPath subset:
//   $.field
//   $.nested.field
//   $.array[0].field
//   $.field[0]
//
// Beyond JSONPath, the detailed variant additionally resolves:
//   - Source::Header     — looks up an HTTP response header (case-insensitive).
//   - Source::StatusCode — captures the response status as a string.
//   - Source::Cookie     — parses Set-Cookie headers and returns the named cookie.
//   - Source::Regex      — runs a regex against the body and returns capture
//                          group 1 (or the whole match when no group).
//   - Source::XPath      — still Unsupported; XML parsing is post-MVP.
#pragma once

#include <chainapi/engine/ErrorCodes.h>
#include <chainapi/engine/Operation.h>
#include <chainapi/engine/RunContext.h>

#include <expected>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace chainapi::engine {

[[nodiscard]] std::expected<std::map<std::string, std::string>, ChainApiError> extractFromJson(
    const std::string& body, const std::vector<Extraction>& extractions);

/// Per-extraction outcome including the resolved value (when present).
/// Unlike `extractFromJson`, this never short-circuits — every entry in
/// `extractions` produces one trace row, so callers that want to record
/// the full extraction history (the executor, for the timeline UI) can.
///
/// Returns `ResponseParse` only when the body is not valid JSON AND at
/// least one extraction needs to look at the body (JsonPath, Regex). A
/// header / status / cookie extraction against a non-JSON body succeeds
/// because none of those need to parse JSON.
///
/// A path that doesn't resolve is NOT an error — it surfaces as
/// `ExtractionTrace::Outcome::Missing`.
struct DetailedExtraction {
    std::vector<ExtractionTrace> traces;
    std::map<std::string, std::string> values;  ///< Resolved values only.
};

/// Body-only entry point. Callers that only have JSON (auth steps,
/// refresh blocks) keep using this — Header / StatusCode / Cookie /
/// Regex all return Unsupported when called this way.
[[nodiscard]] std::expected<DetailedExtraction, ChainApiError> extractFromJsonDetailed(
    const OperationId& opId, const std::string& body, const std::vector<Extraction>& extractions);

/// Full-response entry point used by the executor. Resolves Header /
/// StatusCode / Cookie / Regex sources from the captured response in
/// addition to JSONPath against `body`.
[[nodiscard]] std::expected<DetailedExtraction, ChainApiError> extractFromResponseDetailed(
    const OperationId& opId,
    const std::string& body,
    int statusCode,
    const std::vector<std::pair<std::string, std::string>>& headers,
    const std::vector<Extraction>& extractions);

}  // namespace chainapi::engine
