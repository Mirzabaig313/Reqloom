// Host approval at the terminal (PRD FR-15.9): asks yes/no before a credential first goes to a
// host, as SSH does on first connection. With no terminal (an agent, CI, a pipe) or with CLI
// approval turned off it never asks and never assumes yes.
#pragma once

#include <reqloom/engine/HostApproval.h>

#include <cstdio>
#include <expected>
#include <string>
#include <vector>

namespace reqloom::cli {

/// Environment variable CI sets to approve hosts for one run: comma-separated hosts, each
/// `host` (port 443) or `host:port`. Never stored.
inline constexpr const char* kApprovedHostsVariable = "REQLOOM_APPROVED_HOSTS";

class TerminalHostApprover final : public engine::HostApprover {
public:
    /// `in`/`out` are the terminal (stdin and stderr in the CLI). `interactive` false, or
    /// `cliApprovalAllowed` false, means nobody is asked.
    TerminalHostApprover(bool interactive, bool cliApprovalAllowed, std::FILE* in, std::FILE* out)
        : interactive_(interactive), cliApprovalAllowed_(cliApprovalAllowed), in_(in), out_(out) {}

    engine::HostApprovalAnswer ask(const engine::HostApprovalRequest& request) override;
    [[nodiscard]] std::string howToApprove(
        const engine::HostApprovalRequest& request) const override;

private:
    bool interactive_;
    bool cliApprovalAllowed_;
    std::FILE* in_;
    std::FILE* out_;
};

/// One answer line from `in`, lowercased and trimmed (at most 64 characters kept); empty at end
/// of input.
[[nodiscard]] std::string readAnswerLine(std::FILE* in);

/// True when stdin and stderr are both terminals, so a person can answer a question.
[[nodiscard]] bool terminalAttached() noexcept;

/// The hosts `REQLOOM_APPROVED_HOSTS` approves for this run (empty when unset), or an error naming
/// the bad entry.
[[nodiscard]] std::expected<std::vector<std::string>, std::string> hostsApprovedByEnvironment();

}  // namespace reqloom::cli
