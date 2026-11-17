// `reqloom run` end to end against the mock server: the JSON document carries what the API
// returned, and no credential reaches any output format (PRD FR-13.11a, FR-15.3, NFR-3.7).
// `session.open` returns a session id under ordinary names; `order.pay` sends it as a bearer
// token, and its 422 body quotes it. The id is fake and longer than any per-value cut.

#include "CliTestSupport.h"

#include "commands/RunCommand.h"

#include "MockSutHarness.h"

#include <gtest/gtest.h>

#include <QtCore/QByteArray>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QString>

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace cli = reqloom::cli;
using reqloom::cli::tests::argList;
using reqloom::cli::tests::CapturedRun;
using reqloom::cli::tests::captureRun;
using reqloom::cli::tests::TempDir;

namespace {

[[nodiscard]] std::string fakeSessionId() {
    std::string id{"fake-cli-sid-"};
    while (id.size() < 300) {
        id += "fedcba9876543210";
    }
    return id;
}

/// True when any 12-byte piece of the fake id appears in `text`.
[[nodiscard]] bool leaksToken(const std::string& text) {
    const auto token = fakeSessionId();
    constexpr std::size_t kPiece = 12;
    for (std::size_t i = 13; i + kPiece <= token.size(); i += 4) {
        if (text.find(token.substr(i, kPiece)) != std::string::npos) {
            return true;
        }
    }
    return false;
}

class AgentProject : public TempDir {
public:
    AgentProject() {
        const auto token = fakeSessionId();
        const auto routes = write("routes.json",
                                  R"({"routes":[
  {"method":"POST","path":"/sessions","status":201,
   "body":{"data":{"sid":")" + token + R"(","note":"welcome"}}},
  {"method":"POST","path":"/pay","status":422,
   "body":{"error":"session )" + token +
                                      R"( cannot pay","reason":"card declined"}}
]})");
        sut_ = std::make_unique<reqloom::tests::MockSutHarness>(routes);
        write("reqloom.yaml", R"(version: 1
name: AgentOutput
default_environment: local
environment:
  baseUrl: )" + sut_->baseUrl() + R"(
resources:
  session:
    operations:
      open:
        method: POST
        path: /sessions
        expect_status: 201
        extract:
          sid: $.data.sid
          note: $.data.note
  order:
    operations:
      pay:
        method: POST
        path: /pay
        headers:
          Authorization: "Bearer {{session.sid}}"
        expect_status: 200
)");
    }
    [[nodiscard]] std::string str() const { return path().string(); }

private:
    std::unique_ptr<reqloom::tests::MockSutHarness> sut_;
};

[[nodiscard]] CapturedRun run(const std::vector<std::string>& args) {
    return captureRun([&] { return cli::runCommand(argList(args)); });
}

[[nodiscard]] QJsonObject parseJson(const std::string& text) {
    QJsonParseError error{};
    const auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(text), &error);
    EXPECT_EQ(error.error, QJsonParseError::NoError) << text;
    return doc.object();
}

}  // namespace

TEST(AgentOutput, json_carries_status_body_and_extracted_values_per_step) {
    const AgentProject project;
    const auto r = run({"order.pay", "--project", project.str(), "--format", "json"});
    EXPECT_EQ(r.rc, 1) << r.err;
    const auto doc = parseJson(r.out);
    const auto steps = doc.value(QStringLiteral("steps")).toArray();
    ASSERT_EQ(steps.size(), 2) << r.out;

    const auto open = steps.at(0).toObject();
    EXPECT_EQ(open.value(QStringLiteral("http_status")).toInt(), 201) << r.out;
    const auto openBody = open.value("response").toObject().value("body").toString();
    EXPECT_TRUE(openBody.contains(QStringLiteral("welcome"))) << r.out;
    const auto extracted = open.value(QStringLiteral("extracted")).toArray();
    ASSERT_EQ(extracted.size(), 2) << r.out;
    EXPECT_EQ(extracted.at(1).toObject().value("value").toString(), QStringLiteral("welcome"));

    const auto pay = steps.at(1).toObject();
    EXPECT_EQ(pay.value(QStringLiteral("http_status")).toInt(), 422) << r.out;
    const auto response = pay.value(QStringLiteral("response")).toObject();
    EXPECT_EQ(response.value(QStringLiteral("source")).toString(),
              QStringLiteral("system_under_test"));
    EXPECT_TRUE(response.value(QStringLiteral("body")).toString().contains("card declined"));
    EXPECT_EQ(response.value(QStringLiteral("limit_bytes")).toInt(), 2048);
    EXPECT_FALSE(pay.value(QStringLiteral("next_action")).toString().isEmpty());
    EXPECT_EQ(doc.value("failed_step").toObject().value("op").toString(),
              QStringLiteral("order.pay"));
}

TEST(AgentOutput, token_sent_as_a_credential_appears_in_no_output_format) {
    const AgentProject project;
    for (const auto* format : {"json", "junit", "text"}) {
        const auto r = run({"order.pay", "--project", project.str(), "--format", format});
        EXPECT_EQ(r.rc, 1) << format << r.err;
        EXPECT_FALSE(leaksToken(r.out)) << format << "\n" << r.out;
        EXPECT_FALSE(leaksToken(r.err)) << format << "\n" << r.err;
    }
}

TEST(AgentOutput, response_body_size_flag_sets_the_stated_limit) {
    const AgentProject project;
    const auto r = run({"order.pay",
                        "--project",
                        project.str(),
                        "--format",
                        "json",
                        "--response-body-size",
                        "40"});
    EXPECT_EQ(r.rc, 1) << r.err;
    const auto response =
        parseJson(r.out).value("steps").toArray().at(1).toObject().value("response").toObject();
    EXPECT_EQ(response.value(QStringLiteral("limit_bytes")).toInt(), 40) << r.out;
    EXPECT_TRUE(response.value(QStringLiteral("truncated")).toBool()) << r.out;
    EXPECT_LE(response.value(QStringLiteral("body")).toString().toUtf8().size(), 40) << r.out;
    EXPECT_FALSE(leaksToken(r.out)) << r.out;
}

TEST(AgentOutput, invalid_response_body_size_is_an_argument_error) {
    const AgentProject project;
    const auto r = run({"order.pay", "--project", project.str(), "--response-body-size", "lots"});
    EXPECT_EQ(r.rc, 2);
}

TEST(AgentOutput, cli_reads_secrets_from_the_uppercased_environment_variable) {
    const AgentProject project;
    // A project without an id whose request needs {{secret.api_key}}: on the CLI the value comes
    // from REQLOOM_SECRET_API_KEY, with no keychain involved.
    project.write("reqloom.yaml",
                  reqloom::cli::tests::readText(project.path() / "reqloom.yaml") +
                      "        query_params:\n          api_key: \"{{secret.api_key}}\"\n");
#ifdef _WIN32
    _putenv_s("REQLOOM_SECRET_API_KEY", "fake-cli-env-secret-1");
#else
    setenv("REQLOOM_SECRET_API_KEY", "fake-cli-env-secret-1", 1);
#endif
    const auto r = run({"order.pay", "--project", project.str(), "--format", "json"});
#ifdef _WIN32
    _putenv_s("REQLOOM_SECRET_API_KEY", "");
#else
    unsetenv("REQLOOM_SECRET_API_KEY");
#endif
    EXPECT_EQ(r.rc, 1) << r.err;
    const auto pay = parseJson(r.out).value("steps").toArray().at(1).toObject();
    EXPECT_EQ(pay.value(QStringLiteral("http_status")).toInt(), 422) << r.out;
    EXPECT_EQ(r.out.find("fake-cli-env-secret-1"), std::string::npos) << r.out;
}
