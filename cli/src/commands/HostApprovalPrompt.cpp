// HostApprovalPrompt — see header.
#include "HostApprovalPrompt.h"

#include "../output/TerminalText.h"

#include <QtCore/QByteArray>
#include <QtCore/QtGlobal>

#include <algorithm>
#include <cctype>
#include <format>
#include <print>
#include <string>
#include <string_view>

#if defined(_WIN32)
#include <io.h>
#define REQLOOM_ISATTY _isatty
#define REQLOOM_FILENO _fileno
#else
#include <unistd.h>
#define REQLOOM_ISATTY isatty
#define REQLOOM_FILENO fileno
#endif

namespace reqloom::cli {

namespace {

namespace ce = reqloom::engine;

}  // namespace

std::string readAnswerLine(std::FILE* in) {
    std::string line;
    for (int c = std::fgetc(in); c != EOF && c != '\n'; c = std::fgetc(in)) {
        if (line.size() < 64) {
            line.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
        line.pop_back();
    }
    const auto start = line.find_first_not_of(' ');
    return start == std::string::npos ? std::string{} : line.substr(start);
}

namespace {

[[nodiscard]] std::string projectLabel(const ce::HostApprovalRequest& request) {
    return request.projectId.empty() ? std::format("'{}'", terminalSafe(request.projectName))
                                     : std::format("'{}' (id {})",
                                                   terminalSafe(request.projectName),
                                                   terminalSafe(request.projectId));
}

}  // namespace

ce::HostApprovalAnswer TerminalHostApprover::ask(const ce::HostApprovalRequest& request) {
    if (!interactive_ || !cliApprovalAllowed_ || in_ == nullptr || out_ == nullptr) {
        return ce::HostApprovalAnswer::Unavailable;
    }
    std::print(out_,
               "\nProject {} is about to send {} to\n"
               "    {}\n"
               "  request: {}\n"
               "This host has not been approved for this project. Only approve it if you expect "
               "this project to send credentials there.\n"
               "Approve {} and remember the answer (yes/no)? ",
               projectLabel(request),
               terminalSafe(request.credential),
               terminalSafe(request.host),
               terminalSafe(request.url),
               terminalSafe(request.host));
    std::fflush(out_);
    for (int tries = 0; tries < 3; ++tries) {
        const auto answer = readAnswerLine(in_);
        if (answer == "yes" || answer == "y") {
            std::print(out_, "Approved {} for this project.\n", request.host);
            return ce::HostApprovalAnswer::Approve;
        }
        if (answer == "no" || answer == "n" || answer.empty()) {
            std::print(out_, "Not approved; nothing was sent to {}.\n", request.host);
            return ce::HostApprovalAnswer::Deny;
        }
        std::print(out_, "Please type 'yes' or 'no': ");
        std::fflush(out_);
    }
    return ce::HostApprovalAnswer::Deny;
}

std::string TerminalHostApprover::howToApprove(const ce::HostApprovalRequest& request) const {
    if (!cliApprovalAllowed_) {
        return "Approving hosts from the command line is turned off on this machine: a person "
               "must open the project in the Reqloom app and approve " +
               request.host + " there.";
    }
    return std::format(
        "A person must approve it: run this command in a terminal and answer yes, run `reqloom "
        "hosts approve {}` in a terminal, or open the project in the Reqloom app. A CI pipeline "
        "can approve it for its runs with {}={} in the pipeline's settings.",
        request.host,
        kApprovedHostsVariable,
        request.host);
}

bool terminalAttached() noexcept {
    return REQLOOM_ISATTY(REQLOOM_FILENO(stdin)) != 0 &&
           REQLOOM_ISATTY(REQLOOM_FILENO(stderr)) != 0;
}

std::expected<std::vector<std::string>, std::string> hostsApprovedByEnvironment() {
    const QByteArray value = qgetenv(kApprovedHostsVariable);
    if (value.isEmpty()) {
        return std::vector<std::string>{};
    }
    auto hosts = ce::parseHostList(value.toStdString());
    if (!hosts) {
        return std::unexpected(std::format("{}: {}", kApprovedHostsVariable, hosts.error().detail));
    }
    return std::move(*hosts);
}

}  // namespace reqloom::cli
