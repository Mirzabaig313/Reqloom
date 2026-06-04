// JsonPathEvaluator — JSONPath evaluation against response bodies.
#include "JsonPathEvaluator.h"

namespace reqloom::engine {

std::expected<std::optional<std::string>, ReqloomError> JsonPathEvaluator::evaluate(
    std::string_view /*json*/, std::string_view /*jsonpath*/) const {
    return std::optional<std::string>{};
}

}  // namespace reqloom::engine
