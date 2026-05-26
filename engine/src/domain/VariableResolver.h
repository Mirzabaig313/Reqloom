#pragma once

#include <chainapi/engine/RunContext.h>

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace chainapi::engine {

/// Context that provides environment variables and secrets for resolution.
struct ResolveContext {
    std::map<std::string, std::string> envVars;
    std::map<std::string, std::string> secrets;
};

class VariableResolver {
public:
    VariableResolver();
    ~VariableResolver();

    struct Result {
        std::string output;                   ///< Substituted string.
        std::vector<std::string> unresolved;  ///< {{X.y}} that could not resolve.
    };

    /// Substitute every `{{X.y}}` reference. Unresolved references are
    /// listed; the caller decides whether to fail (live run) or surface
    /// `<UNRESOLVED: X.y>` markers (dry run).
    Result resolve(std::string_view templateStr,
                   const RunContext& ctx,
                   const ResolveContext& resolveCtx) const;
};

}  // namespace chainapi::engine
