#pragma once

#include <reqloom/engine/ErrorCodes.h>

#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace reqloom::engine {

class JsonPathEvaluator {
public:
    /// Returns the matched value as a stringified scalar; nullopt on miss.
    /// Returns `ReqloomError{ResponseParse, ...}` if the JSON itself is
    /// malformed.
    [[nodiscard]] std::expected<std::optional<std::string>, ReqloomError> evaluate(
        std::string_view json, std::string_view jsonpath) const;
};

}  // namespace reqloom::engine
