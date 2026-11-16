// JsonExtraction — apply a list of `Extraction`s against a JSON body; shared by ExecutionEngine and
// AuthStrategy. Supported JSONPath subset:
//   $.field
//   $.nested.field
//   $.array[0].field
//   $.field[0]
//   $.items[?(@.status == 'open')].id   (predicate filter; `@` = element)
//   $.items[*].id                       (list extraction only)
// Anything else (`$..x`, quoted keys, negative indices, an unclosed bracket, a malformed
// filter) is rejected as an unsupported path, never a silent miss. Bodies nested deeper than
// kMaxJsonNestingDepth (256) are rejected with ResponseParse; parse errors report a byte
// offset, never body text.
// Beyond JSONPath, the detailed variant also resolves:
//   - Source::Header     — looks up an HTTP response header (case-insensitive).
//   - Source::StatusCode — captures the response status as a string.
//   - Source::Cookie     — parses Set-Cookie headers and returns the named cookie.
//   - Source::Regex      — runs a regex against the body and returns capture
//                          group 1 (or the whole match when no group).
//   - Source::XPath      — still Unsupported; XML parsing is post-MVP.
#pragma once

#include <reqloom/engine/ErrorCodes.h>
#include <reqloom/engine/Operation.h>
#include <reqloom/engine/RunContext.h>

#include <nlohmann/json_fwd.hpp>

#include <expected>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace reqloom::engine {

/// Resolve a JSONPath (field access, indexing, `[?(@.f=='x')]` first-match filters) against a
/// parsed document. nullptr on a miss or unsupported path; a resolved null yields a pointer to JSON
/// `null`, so callers can tell missing from null. Shared so extraction and verification walk paths
/// the same way.
[[nodiscard]] const nlohmann::json* walkJsonPath(const nlohmann::json& doc,
                                                 std::string_view sourcePath);

/// Parse `body` as JSON and `evaluateJsonPathAll` against it. Returns an empty
/// vector when the body is not valid JSON or nothing matches.
[[nodiscard]] std::vector<std::string> evaluateJsonPathAll(const nlohmann::json& doc,
                                                           std::string_view sourcePath);

[[nodiscard]] std::expected<std::map<std::string, std::string>, ReqloomError> extractFromJson(
    const std::string& body, const std::vector<Extraction>& extractions);

/// Per-extraction outcome including the resolved value. Never short-circuits: every extraction
/// produces one trace row. Returns `ResponseParse` only when the body is invalid JSON AND an
/// extraction needs it (JsonPath, Regex). An unresolved path is not an error; it traces as
/// `Outcome::Missing`.
struct DetailedExtraction {
    std::vector<ExtractionTrace> traces;
    std::map<std::string, std::string> values;  ///< Resolved values only.
};

/// Body-only entry point (auth steps, refresh blocks); Header / StatusCode / Cookie / Regex return
/// Unsupported.
[[nodiscard]] std::expected<DetailedExtraction, ReqloomError> extractFromJsonDetailed(
    const OperationId& opId, const std::string& body, const std::vector<Extraction>& extractions);

/// Full-response entry point used by the executor; also resolves Header / StatusCode / Cookie /
/// Regex sources.
[[nodiscard]] std::expected<DetailedExtraction, ReqloomError> extractFromResponseDetailed(
    const OperationId& opId,
    const std::string& body,
    int statusCode,
    const std::vector<std::pair<std::string, std::string>>& headers,
    const std::vector<Extraction>& extractions);

}  // namespace reqloom::engine
