// `reqloom show` without a server: what it refuses. It reads only the masked rows `reqloom run`
// recorded; a run the desktop app recorded (unmasked bodies in its events) is never printed.
// The end-to-end path (run, then show) is in AgentOutputTest.

#include "CliTestSupport.h"

#include "commands/ShowCommand.h"

#include "infrastructure/storage/HistoryStore.h"

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/JsonValues.h>
#include <reqloom/engine/PublicApi.h>

#include <gtest/gtest.h>

#include <QtCore/QByteArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace cli = reqloom::cli;
namespace ce = reqloom::engine;
using reqloom::cli::tests::argList;
using reqloom::cli::tests::CapturedRun;
using reqloom::cli::tests::captureRun;
using reqloom::cli::tests::TempDir;

namespace {

constexpr const char* kFakeToken = "fake-desktop-token-abcdef0123456789";

class Project : public TempDir {
public:
    Project() {
        write("reqloom.yaml",
              "version: 1\nname: S\nresources:\n  item:\n    operations:\n      read:\n"
              "        path: /x\n");
    }
    [[nodiscard]] std::string str() const { return path().string(); }
};

[[nodiscard]] CapturedRun show(const std::vector<std::string>& args) {
    return captureRun([&] { return cli::showCommand(argList(args)); });
}

[[nodiscard]] QJsonObject parseJson(const std::string& text) {
    QJsonParseError error{};
    const auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(text), &error);
    EXPECT_EQ(error.error, QJsonParseError::NoError) << text;
    return doc.object();
}

/// A run as the desktop app records it: events only, the body as received, a token in it.
void recordDesktopRun(const Project& project, std::uint64_t id) {
    const auto dbPath = ce::defaultHistoryDatabasePath(project.path());
    ASSERT_TRUE(dbPath.has_value());
    auto store = ce::makeSqliteHistoryStore();
    ASSERT_TRUE(store->open(*dbPath).has_value());
    const auto now = std::chrono::system_clock::now();
    const ce::RunId run{id};
    ASSERT_TRUE(
        store->append(ce::RunStarted{run, ce::OperationId{"item.read"}, 1, "local", now, {}})
            .has_value());
    ce::ResponseReceived received;
    received.runId = run;
    received.status = 200;
    received.body = std::string{R"({"token":")"} + kFakeToken + R"("})";
    received.at = now;
    ASSERT_TRUE(store->append(received).has_value());
    ASSERT_TRUE(store->append(ce::RunEnded{run, ce::RunOutcome::Succeeded, {}, now}).has_value());
}

}  // namespace

TEST(ShowCommand, a_project_with_no_history_says_so) {
    const Project project;
    const auto r = show({"last", "0", "--project", project.str(), "--format", "json"});
    EXPECT_EQ(r.rc, 2);
    const auto doc = parseJson(r.out);
    EXPECT_EQ(doc.value("contract_version").toString(), QStringLiteral("1.2"));
    EXPECT_EQ(doc.value("command").toString(), QStringLiteral("show"));
    const auto error = doc.value("error").toObject();
    EXPECT_EQ(error.value("code").toString(), QStringLiteral("E_CLI_NO_HISTORY"));
    EXPECT_FALSE(error.value("next_action").toString().isEmpty());
    EXPECT_FALSE(std::filesystem::exists(*ce::defaultHistoryDatabasePath(project.path())))
        << "show must not create a history file";
}

TEST(ShowCommand, a_run_the_app_recorded_is_refused_and_never_printed) {
    const Project project;
    recordDesktopRun(project, 7001);
    for (const auto* format : {"json", "text"}) {
        auto r = show({"7001", "0", "--project", project.str(), "--format", format});
        EXPECT_EQ(r.rc, 2) << format;
        EXPECT_EQ(r.out.find(kFakeToken), std::string::npos) << r.out;
        EXPECT_EQ(r.err.find(kFakeToken), std::string::npos) << r.err;
        r = show({"last", "0", "--project", project.str(), "--format", format});
        EXPECT_EQ(r.rc, 2) << format;
        EXPECT_EQ(r.out.find(kFakeToken), std::string::npos) << r.out;
    }
    const auto r = show({"7001", "0", "--project", project.str(), "--format", "json"});
    const auto error = parseJson(r.out).value("error").toObject();
    EXPECT_EQ(error.value("code").toString(), QStringLiteral("E_CLI_UNKNOWN_RUN"));
    EXPECT_TRUE(error.value("detail").toString().contains(QStringLiteral("Reqloom app")));
}

TEST(ShowCommand, an_unknown_run_id_is_an_invocation_error) {
    const Project project;
    recordDesktopRun(project, 7002);
    const auto r = show({"99999", "0", "--project", project.str(), "--format", "json"});
    EXPECT_EQ(r.rc, 2);
    EXPECT_EQ(parseJson(r.out).value("error").toObject().value("code").toString(),
              QStringLiteral("E_CLI_UNKNOWN_RUN"));
}

TEST(ShowCommand, missing_arguments_and_bad_size_are_usage_errors) {
    const Project project;
    EXPECT_EQ(show({"last", "--project", project.str()}).rc, 2);
    EXPECT_EQ(show({"last", "0", "--project", project.str(), "--size", "lots"}).rc, 2);
    EXPECT_EQ(show({"--help"}).rc, 0);
}

// `--path` tells "not JSON" from "nothing matched" by asking for `$`, the whole document.
TEST(ShowCommand, the_root_path_selects_any_json_document_and_nothing_else) {
    EXPECT_EQ(ce::extractValues(R"({"a":1})", "$").size(), 1U);
    EXPECT_EQ(ce::extractValues("[1,2]", "$").size(), 1U);
    EXPECT_TRUE(ce::extractValues("plain text, not JSON", "$").empty());
    EXPECT_TRUE(ce::extractValues(R"({"a":)", "$").empty());
}
