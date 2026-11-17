// HostsCommandTest — `reqloom hosts`: approving needs a terminal and CLI approval on; revoking and
// turning CLI approval off are always allowed; turning it on is refused at the command line.
#include "CliTestSupport.h"

#include "commands/HostsCommand.h"

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cli = reqloom::cli;
namespace ce = reqloom::engine;
using reqloom::cli::tests::argList;
using reqloom::cli::tests::captureRun;
using reqloom::cli::tests::TempDir;

namespace {

struct FileCloser {
    void operator()(std::FILE* f) const noexcept { std::fclose(f); }
};

/// A project folder plus its own per-user config directory (through the test seam) for one test.
class HostsRig : public TempDir {
public:
    HostsRig() : previous_(ce::configDirectoryForTesting()) {
        ce::setConfigDirectoryForTesting(path() / "config");
        write("reqloom.yaml",
              "version: 1\nname: Shop\nid: shop-hosts-0001\nenvironment:\n  baseUrl: "
              "https://api.example.com\nresources: {}\n");
    }
    ~HostsRig() { ce::setConfigDirectoryForTesting(previous_); }
    HostsRig(const HostsRig&) = delete;
    HostsRig& operator=(const HostsRig&) = delete;
    HostsRig(HostsRig&&) = delete;
    HostsRig& operator=(HostsRig&&) = delete;

    /// Run `reqloom hosts …` with `answer` typed at the (fake) terminal; `prompt` gets what it
    /// asked.
    [[nodiscard]] reqloom::cli::tests::CapturedRun run(std::vector<std::string> args,
                                                       bool terminal,
                                                       const std::string& answer = "yes\n",
                                                       std::string* prompt = nullptr) const {
        args.emplace_back("--project");
        args.push_back(path().string());
        const std::unique_ptr<std::FILE, FileCloser> in{std::tmpfile()};
        const std::unique_ptr<std::FILE, FileCloser> out{std::tmpfile()};
        std::fputs(answer.c_str(), in.get());
        std::rewind(in.get());
        auto result = captureRun([&] {
            return cli::hostsCommand(
                argList(args), [terminal] { return terminal; }, in.get(), out.get());
        });
        if (prompt != nullptr) {
            std::rewind(out.get());
            prompt->clear();
            for (int c = std::fgetc(out.get()); c != EOF; c = std::fgetc(out.get())) {
                prompt->push_back(static_cast<char>(c));
            }
        }
        return result;
    }

private:
    std::optional<std::filesystem::path> previous_;
};

}  // namespace

TEST(HostsCommand, approve_at_a_terminal_then_list_and_revoke) {
    const HostsRig rig;
    auto r = rig.run({"approve", "API.example.com"}, true);
    ASSERT_EQ(r.rc, 0) << r.err;
    r = rig.run({"list"}, false);
    EXPECT_EQ(r.out, "https://api.example.com:443\n");
    r = rig.run({"revoke", "api.example.com"}, false);
    EXPECT_EQ(r.rc, 0) << r.err;
    r = rig.run({}, false);
    EXPECT_EQ(r.out, "");
}

TEST(HostsCommand, approve_without_a_terminal_is_refused) {
    const HostsRig rig;
    const auto r = rig.run({"approve", "api.example.com"}, false);
    EXPECT_EQ(r.rc, 1);
    EXPECT_NE(r.err.find("terminal"), std::string::npos) << r.err;
    EXPECT_EQ(rig.run({"list"}, false).out, "");
}

TEST(HostsCommand, cli_approval_off_blocks_approve_and_cannot_be_turned_on_here) {
    const HostsRig rig;
    ASSERT_EQ(rig.run({"cli-approval", "off"}, false).rc, 0);
    auto r = rig.run({"approve", "api.example.com"}, true);
    EXPECT_EQ(r.rc, 1);
    EXPECT_NE(r.err.find("Reqloom app"), std::string::npos) << r.err;
    r = rig.run({"cli-approval", "on"}, true);
    EXPECT_EQ(r.rc, 2);
    EXPECT_FALSE(*ce::makeFileHostApprovalStore()->cliApprovalAllowed());
}

TEST(HostsCommand, malformed_host_is_an_invocation_error) {
    const HostsRig rig;
    const auto r = rig.run({"approve", "https://api.example.com/x"}, true);
    EXPECT_EQ(r.rc, 2);
}

TEST(HostsCommand, approve_asks_yes_or_no_and_no_approves_nothing) {
    const HostsRig rig;
    std::string prompt;
    auto r = rig.run({"approve", "api.example.com"}, true, "no\n", &prompt);
    EXPECT_EQ(r.rc, 1);
    EXPECT_NE(prompt.find("Approve https://api.example.com:443"), std::string::npos) << prompt;
    EXPECT_NE(prompt.find("(yes/no)?"), std::string::npos) << prompt;
    EXPECT_EQ(rig.run({"list"}, false).out, "");
    r = rig.run({"approve", "api.example.com"}, true, "", &prompt);  // end of input
    EXPECT_EQ(r.rc, 1);
    EXPECT_EQ(rig.run({"list"}, false).out, "");
}
