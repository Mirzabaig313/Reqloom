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

/// What an operation's request is built from, as far as credentials go (FR-15.9): why it carries
/// one (empty when nothing it refers to is a credential) and the actors whose values it uses.
struct OperationCredentialUse {
    std::string reason;
    std::set<std::string> actors;
};

/// Judged from what the operation refers to, not from the bytes sent, so a transformed value
/// (`{{$.hex.encode(secret.k)}}`) or a hook's rewrite still counts. Counts: a `{{secret.*}}`
/// (directly, through a `!secret` environment value, or read by the pre-request hook), a
/// `{{$.env.*}}`, inline or default auth, any actor's values, and any `<resource>.<field>` in
/// `credentialRefs` (values the project sends as credentials). `actors` lists every actor whose
/// values appear, the operation's own actor included.
[[nodiscard]] OperationCredentialUse operationCredentialUse(
    const Operation& op,
    const Project& project,
    const std::map<std::string, std::string>& envVars,
    const std::set<std::string>& credentialRefs);

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
