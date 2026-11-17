// SecretEnvironment — where `{{secret.NAME}}` comes from when it is not in the OS keychain
// (PRD FR-11.5), and the words a failure uses when it has no value (FR-11.6).
//
// Order, first hit wins: `REQLOOM_SECRET_<NAME>` (see `secretEnvironmentVariable`), then the
// keychain, then unresolved (VarUnresolved naming the secret). A bare `<NAME>` is never read.
#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace reqloom::engine {

/// What the run learned while loading secrets, so a missing one can be explained.
struct SecretSourceState {
    /// True when this run reads `REQLOOM_SECRET_<NAME>` (`RunOptions::secretsFromEnvironment`,
    /// the CLI); the desktop's messages then do not offer the variable as a fix.
    bool environmentEnabled{false};
    /// False when no keychain could be read: none in this build, none configured, or the
    /// keychain reported itself unusable while loading.
    bool backendAvailable{true};
    /// Names whose `REQLOOM_SECRET_<NAME>` is set but empty (treated as unset).
    std::set<std::string> emptyVariables;
};

/// The value of `REQLOOM_SECRET_<name>`; nullopt when the name cannot be mapped, the variable is
/// unset, or it is empty (recorded in `state.emptyVariables`).
[[nodiscard]] std::optional<std::string> readSecretFromEnvironment(const std::string& name,
                                                                   SecretSourceState& state);

/// The first two of `names` that map to one environment variable (`api_key`, `API_KEY`), as
/// "'a' and 'b' both map to REQLOOM_SECRET_X", when that variable is set; else nullopt.
[[nodiscard]] std::optional<std::string> sharedEnvironmentVariable(
    const std::vector<std::string>& names);

/// Why `{{secret.<name>}}` has no value, and what to do: names the variable to set, says when no
/// secret backend exists, and says when the name cannot come from the environment.
[[nodiscard]] std::string missingSecretCause(std::string_view name, const SecretSourceState& state);

/// Secret names `templates` reference: `{{secret.X}}`, a builtin argument `secret.X`, and
/// `{{env.K}}` whose environment value (`envVars`) references a secret. In first-seen order,
/// without duplicates.
[[nodiscard]] std::vector<std::string> referencedSecretNames(
    const std::vector<std::string_view>& templates,
    const std::map<std::string, std::string>& envVars);

}  // namespace reqloom::engine
