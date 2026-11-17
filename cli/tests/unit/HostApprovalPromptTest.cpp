// HostApprovalPromptTest — the terminal question asked before a credential first goes to a host
// (FR-15.9): what it says, how it reads the answer, and that it never asks without a terminal or
// with CLI approval turned off.
#include "commands/HostApprovalPrompt.h"
#include "output/TerminalText.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <memory>
#include <string>

namespace cli = reqloom::cli;
namespace ce = reqloom::engine;

namespace {

struct FileCloser {
    void operator()(std::FILE* f) const noexcept { std::fclose(f); }
};
using File = std::unique_ptr<std::FILE, FileCloser>;

[[nodiscard]] File input(const std::string& text) {
    File f{std::tmpfile()};
    std::fputs(text.c_str(), f.get());
    std::rewind(f.get());
    return f;
}

[[nodiscard]] std::string contents(std::FILE* f) {
    std::rewind(f);
    std::string out;
    for (int c = std::fgetc(f); c != EOF; c = std::fgetc(f)) {
        out.push_back(static_cast<char>(c));
    }
    return out;
}

[[nodiscard]] ce::HostApprovalRequest question() {
    return ce::HostApprovalRequest{"Shop",
                                   "shop-0001",
                                   "api.example.com:443",
                                   "https://api.example.com/v1/login",
                                   "the credentials of actor 'admin'"};
}

}  // namespace

TEST(HostApprovalPrompt, yes_approves_and_the_question_names_project_host_and_credential) {
    const auto in = input("yes\n");
    const File out{std::tmpfile()};
    cli::TerminalHostApprover approver{true, true, in.get(), out.get()};
    EXPECT_EQ(approver.ask(question()), ce::HostApprovalAnswer::Approve);
    const auto text = contents(out.get());
    EXPECT_NE(text.find("'Shop' (id shop-0001)"), std::string::npos) << text;
    EXPECT_NE(text.find("api.example.com:443"), std::string::npos) << text;
    EXPECT_NE(text.find("the credentials of actor 'admin'"), std::string::npos) << text;
    EXPECT_NE(text.find("(yes/no)?"), std::string::npos) << text;
}

TEST(HostApprovalPrompt, no_empty_or_end_of_input_declines) {
    for (const char* answer : {"no\n", "\n", "", "n\n"}) {
        const auto in = input(answer);
        const File out{std::tmpfile()};
        cli::TerminalHostApprover approver{true, true, in.get(), out.get()};
        EXPECT_EQ(approver.ask(question()), ce::HostApprovalAnswer::Deny) << answer;
    }
}

TEST(HostApprovalPrompt, anything_else_is_asked_again_then_declined) {
    const auto in = input("sure\nYES\n");
    const File out{std::tmpfile()};
    cli::TerminalHostApprover approver{true, true, in.get(), out.get()};
    EXPECT_EQ(approver.ask(question()), ce::HostApprovalAnswer::Approve);

    const auto in2 = input("maybe\nok\nfine\nyes\n");
    const File out2{std::tmpfile()};
    cli::TerminalHostApprover approver2{true, true, in2.get(), out2.get()};
    EXPECT_EQ(approver2.ask(question()), ce::HostApprovalAnswer::Deny);
}

TEST(HostApprovalPrompt, without_a_terminal_nobody_is_asked) {
    const auto in = input("yes\n");
    const File out{std::tmpfile()};
    cli::TerminalHostApprover approver{false, true, in.get(), out.get()};
    EXPECT_EQ(approver.ask(question()), ce::HostApprovalAnswer::Unavailable);
    EXPECT_TRUE(contents(out.get()).empty());
    const auto hint = approver.howToApprove(question());
    EXPECT_NE(hint.find("REQLOOM_APPROVED_HOSTS=api.example.com:443"), std::string::npos) << hint;
    EXPECT_NE(hint.find("reqloom hosts approve api.example.com:443"), std::string::npos) << hint;
}

TEST(HostApprovalPrompt, cli_approval_turned_off_never_asks_and_points_to_the_app) {
    const auto in = input("yes\n");
    const File out{std::tmpfile()};
    cli::TerminalHostApprover approver{true, false, in.get(), out.get()};
    EXPECT_EQ(approver.ask(question()), ce::HostApprovalAnswer::Unavailable);
    EXPECT_TRUE(contents(out.get()).empty());
    const auto hint = approver.howToApprove(question());
    EXPECT_NE(hint.find("Reqloom app"), std::string::npos) << hint;
    EXPECT_EQ(hint.find("REQLOOM_APPROVED_HOSTS"), std::string::npos) << hint;
}

// Review finding 4: a project name, credential text or URL from the project file cannot drive
// the terminal (erase the line, move the cursor, fake a host line).
TEST(HostApprovalPrompt, project_controlled_text_cannot_send_terminal_control_sequences) {
    const auto in = input("no\n");
    const File out{std::tmpfile()};
    cli::TerminalHostApprover approver{true, true, in.get(), out.get()};
    auto q = question();
    q.projectName = "Shop\x1b[2K\r    api.trusted.example:443\n\xe2\x80\xae";
    q.credential = "the credentials of actor 'a\x1b]0;x\x07'";
    q.url = "https://api.example.com/\x1b[A";
    (void)approver.ask(q);
    const auto text = contents(out.get());
    EXPECT_EQ(text.find('\x1b'), std::string::npos) << text;
    EXPECT_EQ(text.find('\r'), std::string::npos) << text;
    EXPECT_EQ(text.find('\x07'), std::string::npos) << text;
    EXPECT_EQ(text.find("\xe2\x80\xae"), std::string::npos) << text;
    EXPECT_NE(text.find("\\x1b[2K"), std::string::npos) << text;
    EXPECT_NE(text.find("\\u{202e}"), std::string::npos) << text;
}

TEST(TerminalText, keeps_ordinary_text_and_tabs_and_escapes_the_rest) {
    EXPECT_EQ(cli::terminalSafe("Caf\xc3\xa9\tok"), "Caf\xc3\xa9\tok");
    EXPECT_EQ(cli::terminalSafe("a\nb"), "a\\x0ab");
    EXPECT_EQ(cli::terminalSafe("a\nb", true), "a\nb");
    EXPECT_EQ(cli::terminalSafe("\x7f\xc2\x9b\xff"), "\\x7f\\u{009b}\\xff");
}
