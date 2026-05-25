// JsonExtraction — apply a list of `Extraction`s against a JSON body.
//
// Pulled out of ExecutionEngine.cpp so AuthStrategy implementations can
// share the same extraction logic (an actor's auth chain extracts
// variables from JSON exactly like an operation does).

// JSONPath subset supported (intentionally narrow):
//   $.field
//   $.nested.field
//   $.array[0].field          ← bracketed numeric index
//   $.field[0]
//
// Header / status-code / cookie / xpath / regex extractions are NOT
// handled here; they're for the executor (which has the response
// metadata) to apply. Auth steps don't use those today.
#pragma once

#include <chainapi/engine/ErrorCodes.h>
#include <chainapi/engine/Operation.h>

#include <expected>
#include <map>
#include <string>
#include <vector>

namespace chainapi::engine {

[[nodiscard]] std::expected<std::map<std::string, std::string>, ChainApiError>
extractFromJson(const std::string& body,
                const std::vector<Extraction>& extractions);

}  // namespace chainapi::engine
