// HostGate — host approval at send time (PRD FR-15.9, NFR-3.11, tracker D-21).
//
// Every request the engine sends goes through `GatedHttpClient`, after hooks have run and after
// signing, so the gate sees the URL and headers that will actually be sent. A request that carries
// a credential, a secret or a session cookie is sent only when
//   1. its host is a candidate: for an actor's credentials, a host in the actor's `allowed_hosts`,
//      or, with none declared, the host of the environment's `baseUrl` (the list only restricts);
//   2. and the person approved that host for this project: stored in the per-user store, approved
//      for this run by the embedder (CI), or approved now when the run's `HostApprover` asks.
// The same check runs again on every redirect hop, before the hop is sent.
#pragma once

#include "../infrastructure/http/HttpClient.h"

#include <reqloom/engine/Actor.h>
#include <reqloom/engine/ErrorCodes.h>

#include <cstdint>
#include <expected>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace reqloom::engine {

struct Project;
class HostApprovalStore;
class HostApprover;
class SecretRegistry;

/// Why a request is being sent, as far as credentials go.
struct SendPurpose {
    /// The actors whose credentials or values the request carries (an operation run as an actor,
    /// its sign-in or refresh, an operation whose templates use an actor's values); each one's
    /// `allowed_hosts` restricts where it may go.
    std::vector<const Actor*> actors;
    /// The request carries a credential whatever its bytes look like: an operation run as an
    /// actor or referring to secrets, `$.env`, inline auth, an actor's values or a credential
    /// extraction, a hook given secrets, a sign-in, refresh or token request. Redirect hops are
    /// judged by their content.
    bool carriesCredential{false};
    /// What is carried, in words for the person; empty uses the first actor or "a credential".
    std::string reason;
};

class HostGate {
public:
    struct Config {
        const Project* project{nullptr};
        /// The active environment's `baseUrl`, the candidate host of an actor without
        /// `allowed_hosts`.
        std::string baseUrl;
        const SecretRegistry* registry{nullptr};
        /// Secret values too short for the registry, matched as plain text.
        std::vector<std::string> shortSecrets;
        HostApprovalStore* store{nullptr};
        HostApprover* approver{nullptr};
        std::vector<std::string> approvedForRun;
    };

    explicit HostGate(Config config);

    /// An error when `request` may not be sent; otherwise installs the redirect check on it. Asks
    /// the approver at most once per host per run.
    [[nodiscard]] std::optional<ReqloomError> admit(HttpRequest& request,
                                                    const SendPurpose& purpose);

    /// What credential `request` carries, in words for the person ("a session cookie"), or
    /// nothing when it carries none.
    [[nodiscard]] std::optional<std::string> credentialIn(const HttpRequest& request,
                                                          const SendPurpose& purpose) const;

    /// Why `host` may not receive what `purpose` carries because of an actor's `allowed_hosts` (or
    /// the `baseUrl` rule), or nothing. Never asks; used by `planChain` too.
    [[nodiscard]] std::optional<ReqloomError> checkCandidate(const std::string& host,
                                                             const SendPurpose& purpose) const;

    /// How `host` is approved already, without asking anyone or recording anything; the store's
    /// error when it refuses (another folder claims the project id, unreadable file, no id).
    enum class PriorApproval : std::uint8_t { None, ForRun, Stored };
    [[nodiscard]] std::expected<PriorApproval, ReqloomError> approvedWithoutAsking(
        const std::string& host) const;

private:
    /// `admit` without installing the redirect check (the check a redirect hop runs).
    [[nodiscard]] std::optional<ReqloomError> check(const HttpRequest& request,
                                                    const SendPurpose& purpose);
    [[nodiscard]] std::optional<ReqloomError> checkActorCandidate(const std::string& host,
                                                                  const Actor& actor) const;
    [[nodiscard]] std::optional<ReqloomError> checkApproval(const std::string& host,
                                                            const HttpRequest& request,
                                                            const std::string& credential);

    Config config_;
    std::string baseHost_;
    std::mutex mutex_;
    /// Per-run answers: true approved, false declined (the error is kept beside it).
    std::map<std::string, bool> decided_;
    std::map<std::string, ReqloomError> refusals_;
};

/// Sends through `inner` after `gate` admits the request. A null gate refuses every request.
class GatedHttpClient final : public HttpClient {
public:
    GatedHttpClient(HttpClient& inner, HostGate* gate, SendPurpose purpose)
        : inner_(inner), gate_(gate), purpose_(std::move(purpose)) {}

    std::expected<HttpResponse, ReqloomError> send(const HttpRequest& request) override;

private:
    HttpClient& inner_;
    HostGate* gate_;
    SendPurpose purpose_;
};

}  // namespace reqloom::engine
