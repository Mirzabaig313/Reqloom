// What a pre_request / post_response hook may see, and how its header edits are applied.
#pragma once

#include <reqloom/engine/ExecutionEngine.h>
#include <reqloom/engine/Operation.h>

#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace reqloom::engine {

/// Secret names a hook on `op` may read: those referenced by the operation's templates (directly or
/// via an environment value), its actor's auth/inject templates, and the hook script itself.
[[nodiscard]] std::set<std::string> hookSecretNames(
    const Operation& op,
    const Project& project,
    const std::map<std::string, std::string>& envVars,
    std::string_view script);

/// Secret names a hook script reads literally (`ctx.secret.NAME` / `ctx.secret["NAME"]`).
[[nodiscard]] std::set<std::string> hookScriptSecretNames(std::string_view script);

/// Apply a hook's header edits to the response headers it was shown. `exposed` is the
/// name→value map the hook saw, `mutated` the map it left. Unchanged names keep every
/// original entry (duplicate `Set-Cookie` lines survive); a changed name becomes one entry
/// with the new value; a removed name is dropped; a new name is appended.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> mergeHookHeaders(
    const std::vector<std::pair<std::string, std::string>>& original,
    const std::map<std::string, std::string>& exposed,
    const std::map<std::string, std::string>& mutated);

}  // namespace reqloom::engine
