#pragma once

#include <reqloom/engine/RunContext.h>
#include <reqloom/engine/Transport.h>

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace reqloom::engine {

/// Context that provides environment variables and secrets for resolution.
struct ResolveContext {
    std::map<std::string, std::string> envVars;
    std::map<std::string, std::string> secrets;
    /// Process environment variable names `{{$.env.NAME}}` may read
    /// (Project::allowedEnv). Any other name is left unresolved.
    std::set<std::string> allowedEnv;
    /// Per-environment transport overrides, stamped onto every outbound HttpRequest so all senders
    /// agree.
    TransportConfig transport;
};

/// Why a `$.env.NAME` builtin token did not resolve, as user-facing text,
/// or nullopt when `token` is not a `$.env.` builtin.
[[nodiscard]] std::optional<std::string> envBuiltinFailureReason(std::string_view token,
                                                                 const ResolveContext& rctx);

class VariableResolver {
public:
    VariableResolver();

    // Where the token appeared; resolveUrlPath() distinguishes path, raw query and fragment.
    enum class Component : std::uint8_t {
        Value,
        UrlPath,
        RawQuery,
        Fragment,
    };

    // token excludes `{{` and `}}`; duplicates are retained in encounter order.
    struct UnresolvedOccurrence {
        std::string token;
        Component component{Component::Value};
    };

    struct Result {
        std::string output;
        // Retained as the token-only compatibility view of unresolvedOccurrences.
        std::vector<std::string> unresolved;
        std::vector<UnresolvedOccurrence> unresolvedOccurrences;
    };

    /// Resolve a URL path template without letting values add URL syntax.
    ///
    /// Values in path segments or the raw query are percent-encoded (existing `%HH` kept); a
    /// reference that is the whole path stays raw so response `Location` templates work.
    ///
    /// @param templateStr  Path, optional raw query, and optional fragment.
    /// @param ctx          Current run state used for extracted values.
    /// @param resolveCtx   Environment variables and secrets.
    /// @return Resolved URL path and any unresolved references.
    [[nodiscard]] Result resolveUrlPath(std::string_view templateStr,
                                        const RunContext& ctx,
                                        const ResolveContext& resolveCtx) const;

    /// Substitute every `{{X.y}}` reference; unresolved ones are listed for the caller to fail
    /// (live run) or mark `<UNRESOLVED: X.y>` (dry run).
    [[nodiscard]] Result resolve(std::string_view templateStr,
                                 const RunContext& ctx,
                                 const ResolveContext& resolveCtx) const;
};

}  // namespace reqloom::engine
