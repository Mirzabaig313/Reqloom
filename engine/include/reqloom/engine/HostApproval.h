// Host approval (PRD FR-15.9, NFR-3.11): an actor's credentials, a secret or a session cookie go
// to a host only after this user has approved that host for this project.
//
// The engine checks every request when it is sent (after hooks and signing) and again on every
// redirect hop. Approvals live outside the project folder, in the per-user store that also binds
// each project id to its folder, so editing the project cannot approve anything. Asking the person
// is the embedder's job: the CLI asks at a terminal, the desktop app shows a dialog; both implement
// `HostApprover`.
#pragma once

#include <reqloom/engine/ErrorCodes.h>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace reqloom::engine {

struct Project;

/// The key a host is approved under: `scheme://host:port`, the scheme and host lowercased, a
/// trailing dot dropped, an IPv6 literal in brackets, the port always written out (the scheme's
/// default when the URL has none). `https://API.Example.com/x` → `https://api.example.com:443`,
/// `http://127.0.0.1:3000` → `http://127.0.0.1:3000`; approving one never covers the other
/// scheme. A host with non-ASCII characters is refused: write it in its `xn--` form.
/// Parsed with libcurl's URL parser, the one that sends the request.
[[nodiscard]] std::expected<std::string, ReqloomError> hostKeyForUrl(std::string_view url);

/// The key for a host the person typed (`REQLOOM_APPROVED_HOSTS`, `reqloom hosts approve`):
/// `[http:// | https://]host[:port]`; no scheme means https, no port the scheme's default
/// (`api.example.com` → `https://api.example.com:443`, `http://127.0.0.1:3000`). A path, user
/// name or wildcard is refused.
[[nodiscard]] std::expected<std::string, ReqloomError> hostKeyForEntry(std::string_view entry);

/// Whether `hostKey` matches an `allowed_hosts` entry (`host` or `host:port`, no scheme): same
/// host, and the same port when the entry names one (`api.example.com` allows any port and
/// scheme of that host; the approval still fixes the scheme).
[[nodiscard]] bool allowedHostMatches(std::string_view entry, std::string_view hostKey);

/// Keys of a comma-separated host list (`REQLOOM_APPROVED_HOSTS`); spaces around entries and empty
/// entries are ignored. One bad entry fails the whole list, naming it.
[[nodiscard]] std::expected<std::vector<std::string>, ReqloomError> parseHostList(
    std::string_view text);

/// The per-user record of approved hosts, keyed by project id and bound to the folder recorded for
/// that id: a project in another folder that claims the id gets an error, never the approvals.
class HostApprovalStore {
public:
    HostApprovalStore() = default;
    HostApprovalStore(const HostApprovalStore&) = delete;
    HostApprovalStore& operator=(const HostApprovalStore&) = delete;
    HostApprovalStore(HostApprovalStore&&) = delete;
    HostApprovalStore& operator=(HostApprovalStore&&) = delete;
    virtual ~HostApprovalStore() = default;

    /// Whether `hostKey` is approved for `project`.
    [[nodiscard]] virtual std::expected<bool, ReqloomError> isApproved(
        const Project& project, const std::string& hostKey) = 0;

    /// Record `hostKey` as approved for `project`.
    [[nodiscard]] virtual std::expected<void, ReqloomError> approve(const Project& project,
                                                                    const std::string& hostKey) = 0;

    /// Forget an approval; no error when there was none.
    [[nodiscard]] virtual std::expected<void, ReqloomError> revoke(const Project& project,
                                                                   const std::string& hostKey) = 0;

    /// Every host approved for `project`, sorted.
    [[nodiscard]] virtual std::expected<std::vector<std::string>, ReqloomError> approvedHosts(
        const Project& project) = 0;

    /// The user setting: may the CLI approve hosts (terminal prompt, `REQLOOM_APPROVED_HOSTS`)?
    /// True unless the person turned it off.
    [[nodiscard]] virtual std::expected<bool, ReqloomError> cliApprovalAllowed() = 0;

    [[nodiscard]] virtual std::expected<void, ReqloomError> setCliApprovalAllowed(bool allowed) = 0;
};

/// What the engine asks the person about.
struct HostApprovalRequest {
    std::string projectName;
    std::string projectId;
    std::string host;  ///< The key, e.g. `https://api.example.com:443`, plus any TLS caveat.
    std::string url;   ///< The request URL with secret values masked.
    /// What would be sent, e.g. "the credentials of actor 'admin'", "a secret or credential".
    std::string credential;
};

enum class HostApprovalAnswer : std::uint8_t {
    Approve,      ///< Send, and remember the approval.
    Deny,         ///< The person said no.
    Unavailable,  ///< Nobody can be asked (no terminal, approval turned off).
};

/// Asks the person. Called on the thread that runs the engine, and may block until they answer.
class HostApprover {
public:
    HostApprover() = default;
    HostApprover(const HostApprover&) = delete;
    HostApprover& operator=(const HostApprover&) = delete;
    HostApprover(HostApprover&&) = delete;
    HostApprover& operator=(HostApprover&&) = delete;
    virtual ~HostApprover() = default;

    [[nodiscard]] virtual HostApprovalAnswer ask(const HostApprovalRequest& request) = 0;

    /// One sentence telling the person how to approve `request.host` on this surface; ends the
    /// error when the answer is not Approve.
    [[nodiscard]] virtual std::string howToApprove(const HostApprovalRequest& request) const = 0;
};

}  // namespace reqloom::engine
