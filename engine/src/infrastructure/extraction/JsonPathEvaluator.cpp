// JsonPathEvaluator — JSONPath evaluation against response bodies.
#include "JsonPathEvaluator.h"

namespace chainapi::engine {

std::expected<std::optional<std::string>, ChainApiError> JsonPathEvaluator::evaluate(
    std::string_view /*json*/, std::string_view /*jsonpath*/) const {
    // Phase 1: backed by nlohmann/json + a JSONPath helper.
    return std::optional<std::string>{};
}

}  // namespace chainapi::engine
