// Public, stateless predicate/assertion evaluation.
//
// Exposes the same boolean expression grammar the engine applies to
// `assertions:` and `poll_until.success_when`, so an editor can offer a
// live "test this expression" box without running a full request.
#pragma once

#include <reqloom/engine/ErrorCodes.h>

#include <expected>
#include <string_view>

namespace reqloom::engine {

/// Evaluate a single assertion / poll predicate against a sample response
/// body and HTTP status, using the engine's predicate grammar
/// (`$.field == 'x'`, `&&`/`||`, `in`, `matches`, etc.).
///
/// Returns the boolean outcome (`true` = the predicate holds) for a
/// well-formed expression, or `ReqloomError{SchemaInvalid}` when the
/// expression is syntactically malformed — letting the editor distinguish a
/// failing assertion from a typo. A structurally valid expression that simply
/// doesn't match (or a non-JSON body) yields `false`, never an error, exactly
/// as it would at run time.
///
/// `statusCode` is exposed inside the expression as `$.status_code`; pass 0
/// when there is no associated HTTP status. Pure: no I/O, no shared state.
[[nodiscard]] std::expected<bool, ReqloomError> evaluatePredicate(std::string_view expression,
                                                                  std::string_view responseBody,
                                                                  int statusCode = 0);

/// Parse-only check of a predicate: `ReqloomError{SchemaInvalid}` with the parser's message
/// when malformed. Used at project load so a bad assertion is rejected before any request.
[[nodiscard]] std::expected<void, ReqloomError> validatePredicate(std::string_view expression);

/// Check a regex the engine will run over response text: at most 1024 bytes, no repeated
/// group ending in an unbounded repeat (`(a+)+`; a heuristic, not a proof), and valid
/// ECMAScript syntax. `ReqloomError{SchemaInvalid}` otherwise.
[[nodiscard]] std::expected<void, ReqloomError> validateRegexPattern(std::string_view pattern);

}  // namespace reqloom::engine
