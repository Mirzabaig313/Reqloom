// Public, stateless predicate/assertion evaluation.
//
// Exposes the grammar the engine applies to `assertions:` and `poll_until.success_when`, so an
// editor can offer a live "test this expression" box.
#pragma once

#include <reqloom/engine/ErrorCodes.h>

#include <expected>
#include <string_view>

namespace reqloom::engine {

/// Evaluate an assertion / poll predicate against a sample response body and HTTP status.
///
/// Returns the boolean outcome for a well-formed expression, or `ReqloomError{SchemaInvalid}` when
/// it is syntactically malformed. A valid expression that does not match (or a non-JSON body)
/// yields `false`, as at run time.
///
/// `statusCode` is exposed as `$.status_code`; pass 0 when there is no HTTP status. Pure.
[[nodiscard]] std::expected<bool, ReqloomError> evaluatePredicate(std::string_view expression,
                                                                  std::string_view responseBody,
                                                                  int statusCode = 0);

/// Parse-only check of a predicate: `ReqloomError{SchemaInvalid}` with the parser's message
/// when malformed. Used at project load so a bad assertion is rejected before any request.
[[nodiscard]] std::expected<void, ReqloomError> validatePredicate(std::string_view expression);

/// `validatePredicate` for a predicate that may contain `{{…}}` placeholders: each is replaced
/// by the neutral value the run-time substitution would produce (a string, quoted outside a
/// literal), so syntax errors around a placeholder are still caught at load.
[[nodiscard]] std::expected<void, ReqloomError> validateTemplatedPredicate(
    std::string_view expression);

/// Check a regex the engine will run over response text: at most 1024 bytes, no repeated
/// group ending in an unbounded repeat (`(a+)+`; a heuristic, not a proof), and valid
/// ECMAScript syntax. `ReqloomError{SchemaInvalid}` otherwise.
[[nodiscard]] std::expected<void, ReqloomError> validateRegexPattern(std::string_view pattern);

/// Check a JSONPath that selects one value, in the subset `extract` supports (`$.a.b`,
/// `$.a[0]`, `$.a[?(@.k == 'v')]`). `[*]`, `$..x` and other unsupported syntax give
/// `ReqloomError{SchemaInvalid}` naming the path and the reason.
[[nodiscard]] std::expected<void, ReqloomError> validateSingleValueJsonPath(std::string_view path);

}  // namespace reqloom::engine
