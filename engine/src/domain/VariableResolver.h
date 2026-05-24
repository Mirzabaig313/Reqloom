#pragma once

#include <chainapi/engine/RunContext.h>

#include <string>
#include <string_view>
#include <vector>

namespace chainapi::engine {

class VariableResolver {
public:
    struct Result {
        std::string output;                   ///< Substituted string.
        std::vector<std::string> unresolved;  ///< {{X.y}} that could not resolve.
    };

    /// Substitute every `{{X.y}}` reference. Unresolved references are
    /// listed; the caller decides whether to fail (live run) or surface
    /// `<UNRESOLVED: X.y>` markers (dry run).
    Result resolve(std::string_view templateStr, const RunContext& ctx) const;
};

}  // namespace chainapi::engine
