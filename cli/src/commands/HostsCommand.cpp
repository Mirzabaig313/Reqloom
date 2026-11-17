// `reqloom hosts` — see header.
#include "HostsCommand.h"

#include "ArgParser.h"
#include "ExitCodes.h"
#include "HostApprovalPrompt.h"
#include "PathText.h"

#include "../output/TerminalText.h"

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/PublicApi.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <print>
#include <string>
#include <string_view>

namespace reqloom::cli {

namespace {

namespace ce = reqloom::engine;
namespace fs = std::filesystem;

constexpr std::array kOptions{
    OptionSpec{.name = "--project", .takesValue = true},
};

void printUsage(std::FILE* stream) {
    std::print(stream,
               "Usage: reqloom hosts [list]                 Hosts approved for the project\n"
               "       reqloom hosts approve <host[:port]>  Approve a host (needs a terminal)\n"
               "       reqloom hosts revoke <host[:port]>   Forget an approval\n"
               "       reqloom hosts cli-approval off       Let only the Reqloom app approve\n"
               "  --project <path>   Project directory (default: cwd)\n"
               "A host is [http://|https://]host[:port]; no scheme means https. Credentials,\n"
               "secrets, session cookies and uploaded files\n"
               "go only to approved hosts. Approvals live in your user settings, never in the\n"
               "project, so editing the project cannot approve a host. Turning command-line\n"
               "approval back on is done in the Reqloom app.\n");
}

[[nodiscard]] int fail(int code, const std::string& message) {
    std::println(stderr, "reqloom hosts: {}", terminalSafe(message));
    return code;
}

}  // namespace

int hostsCommand(const QStringList& args,
                 const std::function<bool()>& terminal,
                 std::FILE* in,
                 std::FILE* out) {
    const auto parsed = parseArguments(args, kOptions);
    if (!parsed) {
        printUsage(stderr);
        return fail(kExitUsage, parsed.error());
    }
    if (parsed->help) {
        printUsage(stdout);
        return kExitSuccess;
    }
    const auto& words = parsed->positionals;
    const std::string verb = words.empty() ? "list" : words.front();
    const auto store = ce::makeFileHostApprovalStore();

    if (verb == "cli-approval") {
        if (words.size() != 2 || (words[1] != "off" && words[1] != "on")) {
            return fail(kExitUsage, "use `reqloom hosts cli-approval off`");
        }
        if (words[1] == "on") {
            return fail(kExitUsage,
                        "command-line approval can be turned back on only in the Reqloom app, so "
                        "a program running commands cannot turn it on");
        }
        if (auto done = store->setCliApprovalAllowed(false); !done) {
            return fail(kExitSystem, done.error().detail);
        }
        std::println("Command-line host approval is off: only the Reqloom app can approve hosts.");
        return kExitSuccess;
    }

    const fs::path folder =
        parsed->value("--project") ? pathFromUtf8(*parsed->value("--project")) : fs::current_path();
    auto project = ce::parseProject(folder / "reqloom.yaml");
    if (!project) {
        return fail(kExitUsage, project.error().detail);
    }

    if (verb == "list" && words.size() <= 1) {
        auto hosts = store->approvedHosts(*project);
        if (!hosts) {
            return fail(kExitFailed, hosts.error().detail);
        }
        for (const auto& host : *hosts) {
            std::println("{}", terminalSafe(host));
        }
        return kExitSuccess;
    }
    if ((verb == "approve" || verb == "revoke") && words.size() == 2) {
        auto host = ce::hostKeyForEntry(words[1]);
        if (!host) {
            return fail(kExitUsage, host.error().detail);
        }
        if (verb == "revoke") {
            if (auto done = store->revoke(*project, *host); !done) {
                return fail(kExitFailed, done.error().detail);
            }
            std::println("Revoked {} for project '{}'.", *host, terminalSafe(project->name));
            return kExitSuccess;
        }
        if (!store->cliApprovalAllowed().value_or(false)) {
            return fail(kExitFailed,
                        "approving hosts from the command line is turned off on this machine; "
                        "approve it in the Reqloom app");
        }
        if (!terminal()) {
            return fail(kExitFailed,
                        "approving a host needs a person at a terminal; run this command in a "
                        "terminal yourself");
        }
        std::print(out,
                   "Approve {} to receive the credentials, secrets and session cookies of "
                   "project '{}'{} (yes/no)? ",
                   terminalSafe(*host),
                   terminalSafe(project->name),
                   project->id.empty() ? std::string{} : " (id " + terminalSafe(project->id) + ")");
        std::fflush(out);
        if (const auto answer = readAnswerLine(in); answer != "yes" && answer != "y") {
            std::println(out, "Not approved.");
            return kExitFailed;
        }
        if (auto done = store->approve(*project, *host); !done) {
            return fail(kExitFailed, done.error().detail);
        }
        std::println("Approved {} for project '{}'.", *host, terminalSafe(project->name));
        return kExitSuccess;
    }
    printUsage(stderr);
    return fail(kExitUsage, "unknown hosts command");
}

int hostsCommand(const QStringList& args) {
    return hostsCommand(args, [] { return terminalAttached(); }, stdin, stderr);
}

}  // namespace reqloom::cli
