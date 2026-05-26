// JsonExtraction — apply a list of `Extraction`s against a JSON body.
//
// Pulled out of ExecutionEngine.cpp so AuthStrategy implementations can
// share the same extraction logic.
//
// Supported JSONPath subset:
//   $.field
//   $.nested.field
//   $.array[0].field
//   $.field[0]
//
// Header / status-code / cookie / xpath / regex extractions are not
// handled here; they require response metadata the executor owns.
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
