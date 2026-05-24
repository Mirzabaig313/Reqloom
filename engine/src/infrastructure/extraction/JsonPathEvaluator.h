#pragma once

#include <chainapi/engine/ErrorCodes.h>

#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace chainapi::engine {

class JsonPathEvaluator {
public:
    /// Returns the matched value as a stringified scalar; nullopt on miss.
    /// Returns `ChainApiError{ResponseParse, ...}` if the JSON itself is
    /// malformed.
    std::expected<std::optional<std::string>, ChainApiError> evaluate(
        std::string_view json, std::string_view jsonpath) const;
};

}  // namespace chainapi::engine
