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

#include <expected>
#include <map>
#include <string>
#include <vector>

namespace chainapi::engine {

[[nodiscard]] std::expected<std::map<std::string, std::string>, ChainApiError>
extractFromJson(const std::string& body,
                const std::vector<Extraction>& extractions);

}  // namespace chainapi::engine
