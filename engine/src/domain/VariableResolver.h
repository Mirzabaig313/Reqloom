#pragma once

#include <reqloom/engine/RunContext.h>
#include <reqloom/engine/Transport.h>

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace reqloom::engine {

/// Context that provides environment variables and secrets for resolution.
struct ResolveContext {
    std::map<std::string, std::string> envVars;
    std::map<std::string, std::string> secrets;
    /// Per-environment transport overrides. The executor stamps this
    /// onto every outbound HttpRequest before send so authenticators,
    /// poll loops, and the main step builder all see the same TLS /
    /// proxy / connect-timeout settings.
    TransportConfig transport;
};

class VariableResolver {
public:
    VariableResolver();

    struct Result {
        std::string output;                   ///< Substituted string.
        std::vector<std::string> unresolved;  ///< {{X.y}} that could not resolve.
    };

    /// Resolve a URL path template without letting values add URL syntax.
    ///
    /// Literal URL bytes and separators remain unchanged. Resolved references
    /// embedded in path segments or the raw query are percent-encoded while
    /// existing `%HH` escapes stay intact. A reference that occupies the whole
    /// path remains raw so response `Location` templates keep working.
    ///
    /// @param templateStr  Path, optional raw query, and optional fragment.
    /// @param ctx          Current run state used for extracted values.
    /// @param resolveCtx   Environment variables and secrets.
    /// @return Resolved URL path and any unresolved references.
    [[nodiscard]] Result resolveUrlPath(std::string_view templateStr,
                                        const RunContext& ctx,
                                        const ResolveContext& resolveCtx) const;

    /// Substitute every `{{X.y}}` reference. Unresolved references are
    /// listed; the caller decides whether to fail (live run) or surface
    /// `<UNRESOLVED: X.y>` markers (dry run).
    [[nodiscard]] Result resolve(std::string_view templateStr,
                                 const RunContext& ctx,
                                 const ResolveContext& resolveCtx) const;
};

}  // namespace reqloom::engine
