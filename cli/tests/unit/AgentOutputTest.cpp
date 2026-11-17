// `reqloom run` end to end against the mock server: the JSON document carries what the API
// returned, and no credential reaches any output format (PRD FR-13.11a, FR-15.3, NFR-3.7).
// `session.open` returns a session id under ordinary names; `order.pay` sends it as a bearer
// token, and its 422 body quotes it. The id is fake and longer than any per-value cut.

#include "CliTestSupport.h"

#include "commands/RunCommand.h"

#include <reqloom/engine/Factories.h>
#include <reqloom/engine/HostApproval.h>

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
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cli = reqloom::cli;
using reqloom::cli::tests::argList;
using reqloom::cli::tests::CapturedRun;
using reqloom::cli::tests::captureRun;
using reqloom::cli::tests::TempDir;

namespace {

/// Sets an environment variable for one scope and restores the previous value.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const std::string& value) : name_(name) {
        if (const char* old = std::getenv(name); old != nullptr) {
            previous_ = old;
        }
        set(value);
    }
    ~ScopedEnv() { previous_ ? set(*previous_) : unset(); }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;
    ScopedEnv(ScopedEnv&&) = delete;
    ScopedEnv& operator=(ScopedEnv&&) = delete;

private:
    void set(const std::string& value) const {
#ifdef _WIN32
        _putenv_s(name_, value.c_str());
#else
        setenv(name_, value.c_str(), 1);
#endif
    }
    void unset() const {
#ifdef _WIN32
        _putenv_s(name_, "");
#else
        unsetenv(name_);
#endif
    }
    const char* name_;
    std::optional<std::string> previous_;
};

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
        // The bearer header is a credential: CI-style approval of the mock server's host.
        approved_ = std::make_unique<ScopedEnv>("REQLOOM_APPROVED_HOSTS", host());
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
    [[nodiscard]] std::string host() const {
        return "http://127.0.0.1:" + std::to_string(sut_->port());
    }
    void withdrawApproval() { approved_.reset(); }
    /// Give the project an id, which host approvals are keyed by.
    void giveId() const {
        auto text = reqloom::cli::tests::readText(path() / "reqloom.yaml");
        text.replace(text.find("name: AgentOutput\n"),
                     std::string_view{"name: AgentOutput\n"}.size(),
                     "name: AgentOutput\nid: agent-host-0001\n");
        write("reqloom.yaml", text);
    }

private:
    std::unique_ptr<reqloom::tests::MockSutHarness> sut_;
    std::unique_ptr<ScopedEnv> approved_;
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

// ─── Host approval (FR-15.9) ────────────────────────────────────────────────

TEST(AgentOutput, credential_to_an_unapproved_host_fails_before_sending_and_names_the_fix) {
    AgentProject project;
    project.withdrawApproval();
    project.giveId();
    const auto r = run({"order.pay", "--project", project.str(), "--format", "json"});
    EXPECT_EQ(r.rc, 1) << r.err;
    const auto doc = parseJson(r.out);
    const auto pay = doc.value("steps").toArray().at(1).toObject();
    EXPECT_EQ(pay.value("error_code").toString(), QStringLiteral("E_HOST_NOT_APPROVED")) << r.out;
    const auto detail = pay.value("detail").toString().toStdString();
    EXPECT_NE(detail.find(project.host()), std::string::npos) << detail;
    EXPECT_NE(detail.find("REQLOOM_APPROVED_HOSTS=" + project.host()), std::string::npos) << detail;
    EXPECT_NE(detail.find("reqloom hosts approve"), std::string::npos) << detail;
    EXPECT_TRUE(pay.value("http_status").isNull()) << "the step reached the server: " << r.out;
    EXPECT_NE(pay.value("next_action").toString().indexOf(QStringLiteral("Ask the person")), -1)
        << r.out;
}

TEST(AgentOutput, cli_approval_turned_off_ignores_the_environment_variable) {
    AgentProject project;  // REQLOOM_APPROVED_HOSTS names the host
    project.giveId();
    const reqloom::cli::tests::TempDir config;
    const auto previousDir = reqloom::engine::configDirectoryForTesting();
    reqloom::engine::setConfigDirectoryForTesting(config.path());
    struct Restore {
        std::optional<std::filesystem::path> dir;
        ~Restore() { reqloom::engine::setConfigDirectoryForTesting(dir); }
    } const restore{previousDir};
    ASSERT_TRUE(reqloom::engine::makeFileHostApprovalStore()->setCliApprovalAllowed(false));
    const auto r = run({"order.pay", "--project", project.str(), "--format", "json"});
    EXPECT_EQ(r.rc, 1) << r.err;
    EXPECT_NE(r.err.find("REQLOOM_APPROVED_HOSTS is ignored"), std::string::npos) << r.err;
    const auto pay = parseJson(r.out).value("steps").toArray().at(1).toObject();
    const auto detail = pay.value("detail").toString().toStdString();
    EXPECT_NE(detail.find("Reqloom app"), std::string::npos) << detail;
    EXPECT_EQ(detail.find("REQLOOM_APPROVED_HOSTS"), std::string::npos) << detail;
}

TEST(AgentOutput, malformed_approved_hosts_variable_is_an_invocation_error) {
    AgentProject project;
    const ScopedEnv bad{"REQLOOM_APPROVED_HOSTS", "https://evil.example/x"};
    const auto r = run({"order.pay", "--project", project.str(), "--format", "json"});
    EXPECT_EQ(r.rc, 2) << r.err;
    EXPECT_NE(r.out.find("REQLOOM_APPROVED_HOSTS"), std::string::npos) << r.out;
}

// Coordinator item 1, at the CLI: an environment secret bound for a host nobody approved never
// leaves the process.
TEST(AgentOutput, environment_secret_is_not_sent_to_an_unapproved_host) {
    AgentProject project;
    project.withdrawApproval();
    project.giveId();
    // The secret is the only credential session.open carries, under an ordinary name.
    auto yaml = reqloom::cli::tests::readText(project.path() / "reqloom.yaml");
    const std::string anchor = "        path: /sessions\n";
    yaml.insert(yaml.find(anchor) + anchor.size(),
                "        query_params:\n          ref: \"{{secret.api_key}}\"\n");
    project.write("reqloom.yaml", yaml);
    const ScopedEnv secret{"REQLOOM_SECRET_API_KEY", "fake-cli-env-secret-2"};
    const auto r =
        run({"session.open", "--project", project.str(), "--format", "json", "--non-interactive"});
    EXPECT_EQ(r.rc, 1) << r.err << r.out;
    const auto step = parseJson(r.out).value("steps").toArray().at(0).toObject();
    EXPECT_EQ(step.value("error_code").toString(), QStringLiteral("E_HOST_NOT_APPROVED")) << r.out;
    EXPECT_TRUE(step.value("http_status").isNull()) << "the request reached the server: " << r.out;
    EXPECT_EQ(r.out.find("fake-cli-env-secret-2"), std::string::npos) << r.out;
    EXPECT_EQ(r.err.find("fake-cli-env-secret-2"), std::string::npos) << r.err;
}
