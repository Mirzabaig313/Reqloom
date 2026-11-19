// `reqloom list`, `describe` and `plan`: the read-only commands an agent uses to understand a
// project (PRD FR-13.7–13.9, FR-13.11). They print templates and names, never values, send
// nothing, and write nothing.

#include "CliTestSupport.h"

#include "commands/InspectCommands.h"

#include <reqloom/engine/Factories.h>

#include <gtest/gtest.h>

#include <QtCore/QByteArray>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QtEnvironmentVariables>

#include <filesystem>
#include <string>
#include <vector>

namespace cli = reqloom::cli;
using reqloom::cli::tests::argList;
using reqloom::cli::tests::CapturedRun;
using reqloom::cli::tests::captureRun;
using reqloom::cli::tests::TempDir;

namespace {

constexpr const char* kFakeSecretValue = "fake-inspect-secret-value-0042";

constexpr const char* kShop = R"YAML(version: 1
name: Shop
id: inspect-shop-0001
environment:
  baseUrl: https://api.shop.test
  admin_password: !secret ADMIN_PASSWORD
actors:
  admin:
    auth:
      strategy: simple
      method: POST
      path: /login
      body:
        password: "{{env.admin_password}}"
      extract:
        token: $.token
    inject:
      headers:
        Authorization: "Bearer {{admin.token}}"
resources:
  order:
    operations:
      create:
        method: POST
        path: /orders
        actor: admin
        headers:
          X-Api-Key: "{{secret.ORDER_KEY}}"
        extract:
          order_id: $.id
        expect_status: 201
      pay:
        method: POST
        path: /orders/{{order.order_id}}/pay
        actor: admin
        assert:
          - "$.status == \"paid\""
)YAML";

class ShopProject : public TempDir {
public:
    ShopProject() { write("reqloom.yaml", kShop); }
    [[nodiscard]] std::string str() const { return path().string(); }
};

[[nodiscard]] QJsonObject parseJson(const std::string& text) {
    QJsonParseError error{};
    const auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(text), &error);
    EXPECT_EQ(error.error, QJsonParseError::NoError) << text;
    return doc.object();
}

[[nodiscard]] CapturedRun list(const std::vector<std::string>& args) {
    return captureRun([&] { return cli::listCommand(argList(args)); });
}
[[nodiscard]] CapturedRun describe(const std::vector<std::string>& args) {
    return captureRun([&] { return cli::describeCommand(argList(args)); });
}
[[nodiscard]] CapturedRun plan(const std::vector<std::string>& args) {
    return captureRun([&] { return cli::planCommand(argList(args)); });
}

void expectContract(const QJsonObject& doc, const char* command) {
    EXPECT_EQ(doc.value(QStringLiteral("schema_version")).toInt(), 1);
    EXPECT_EQ(doc.value(QStringLiteral("contract_version")).toString(), QStringLiteral("1.2"));
    EXPECT_EQ(doc.value(QStringLiteral("command")).toString(), QString::fromUtf8(command));
}

}  // namespace

TEST(ListCommand, json_lists_actors_resources_and_operations_with_method_path_and_line) {
    const ShopProject project;
    const auto r = list({"--project", project.str(), "--format", "json"});
    EXPECT_EQ(r.rc, 0) << r.err;
    const auto doc = parseJson(r.out);
    expectContract(doc, "list");
    EXPECT_TRUE(doc.value(QStringLiteral("ok")).toBool());
    const auto actors = doc.value(QStringLiteral("actors")).toArray();
    ASSERT_EQ(actors.size(), 1);
    EXPECT_EQ(actors.at(0).toObject().value("strategy").toString(), QStringLiteral("simple"));
    const auto resources = doc.value(QStringLiteral("resources")).toArray();
    ASSERT_EQ(resources.size(), 1);
    const auto ops = resources.at(0).toObject().value("operations").toArray();
    ASSERT_EQ(ops.size(), 2);
    const auto create = ops.at(0).toObject();
    EXPECT_EQ(create.value("id").toString(), QStringLiteral("order.create"));
    EXPECT_EQ(create.value("method").toString(), QStringLiteral("POST"));
    EXPECT_EQ(create.value("path").toString(), QStringLiteral("/orders"));
    EXPECT_EQ(create.value("actor").toString(), QStringLiteral("admin"));
    EXPECT_EQ(create.value("file").toString(), QStringLiteral("reqloom.yaml"));
    EXPECT_GT(create.value("line").toInt(), 0);
}

TEST(ListCommand, text_lists_each_operation_on_one_line) {
    const ShopProject project;
    const auto r = list({"--project", project.str()});
    EXPECT_EQ(r.rc, 0) << r.err;
    EXPECT_NE(r.out.find("order.pay"), std::string::npos) << r.out;
    EXPECT_NE(r.out.find("/orders/{{order.order_id}}/pay"), std::string::npos) << r.out;
    EXPECT_NE(r.out.find("admin (simple)"), std::string::npos) << r.out;
}

TEST(ListCommand, missing_project_gives_an_error_document_with_a_next_action) {
    const TempDir empty;
    const auto r = list({"--project", empty.path().string(), "--format", "json"});
    EXPECT_EQ(r.rc, 2);
    const auto doc = parseJson(r.out);
    expectContract(doc, "list");
    EXPECT_FALSE(doc.value(QStringLiteral("ok")).toBool());
    const auto error = doc.value(QStringLiteral("error")).toObject();
    EXPECT_EQ(error.value("code").toString(), QStringLiteral("E_CLI_PROJECT_NOT_FOUND"));
    EXPECT_FALSE(error.value("next_action").toString().isEmpty());
}

TEST(ListCommand, invalid_project_error_carries_its_position) {
    TempDir project;
    project.write("reqloom.yaml",
                  "version: 1\nname: P\nresources:\n  item:\n    operations:\n      read:\n"
                  "        path: /x/{{ghost.id}}\n");
    const auto r = list({"--project", project.path().string(), "--format", "json"});
    EXPECT_EQ(r.rc, 1);
    const auto error = parseJson(r.out).value(QStringLiteral("error")).toObject();
    EXPECT_EQ(error.value("code").toString(), QStringLiteral("E_REF_UNDEFINED"));
    EXPECT_EQ(error.value("line").toInt(), 7) << r.out;
}

TEST(DescribeCommand, json_gives_inputs_with_their_source_outputs_and_dependents) {
    const ShopProject project;
    auto r = describe({"order.pay", "--project", project.str(), "--format", "json"});
    EXPECT_EQ(r.rc, 0) << r.err;
    auto doc = parseJson(r.out);
    expectContract(doc, "describe");
    auto op = doc.value(QStringLiteral("operation")).toObject();
    EXPECT_EQ(op.value("actor").toString(), QStringLiteral("admin"));
    const auto consumes = op.value("consumes").toArray();
    ASSERT_EQ(consumes.size(), 1) << r.out;
    const auto use = consumes.at(0).toObject();
    EXPECT_EQ(use.value("token").toString(), QStringLiteral("order.order_id"));
    EXPECT_EQ(use.value("source").toString(), QStringLiteral("extraction"));
    EXPECT_EQ(use.value("producers").toArray().at(0).toString(), QStringLiteral("order.create"));
    EXPECT_EQ(op.value("assertions").toArray().size(), 1);
    EXPECT_EQ(op.value("depends_on").toArray().at(0).toString(), QStringLiteral("order.create"));

    r = describe({"order.create", "--project", project.str(), "--format", "json"});
    op = parseJson(r.out).value(QStringLiteral("operation")).toObject();
    EXPECT_EQ(op.value("dependents").toArray().at(0).toString(), QStringLiteral("order.pay"));
    EXPECT_EQ(op.value("extracts").toArray().at(0).toObject().value("variable").toString(),
              QStringLiteral("order.order_id"));
}

TEST(DescribeCommand, unknown_operation_is_an_invocation_error_listing_the_known_ones) {
    const ShopProject project;
    const auto r = describe({"order.nope", "--project", project.str(), "--format", "json"});
    EXPECT_EQ(r.rc, 2);
    const auto error = parseJson(r.out).value(QStringLiteral("error")).toObject();
    EXPECT_EQ(error.value("code").toString(), QStringLiteral("E_CLI_UNKNOWN_OPERATION"));
    EXPECT_TRUE(error.value("detail").toString().contains(QStringLiteral("order.pay")));
}

TEST(DescribeCommand, missing_operation_argument_is_a_usage_error) {
    const ShopProject project;
    const auto r = describe({"--project", project.str(), "--format", "json"});
    EXPECT_EQ(r.rc, 2);
    EXPECT_EQ(parseJson(r.out).value("error").toObject().value("code").toString(),
              QStringLiteral("E_CLI_USAGE"));
}

TEST(PlanCommand, json_lists_steps_sessions_secret_names_and_hosts_without_values) {
    const ShopProject project;
    qputenv("REQLOOM_SECRET_ORDER_KEY", kFakeSecretValue);
    const auto r = plan({"order.pay", "--project", project.str(), "--format", "json"});
    qunsetenv("REQLOOM_SECRET_ORDER_KEY");
    EXPECT_EQ(r.rc, 0) << r.err;
    EXPECT_EQ(r.out.find(kFakeSecretValue), std::string::npos) << r.out;
    const auto doc = parseJson(r.out);
    expectContract(doc, "plan");
    const auto steps = doc.value(QStringLiteral("steps")).toArray();
    ASSERT_EQ(steps.size(), 2) << r.out;
    EXPECT_EQ(steps.at(0).toObject().value("op").toString(), QStringLiteral("order.create"));
    EXPECT_EQ(steps.at(0).toObject().value("order").toInt(), 1);
    EXPECT_TRUE(steps.at(1).toObject().value("target").toBool());
    const auto sessions = doc.value(QStringLiteral("sessions")).toArray();
    ASSERT_EQ(sessions.size(), 1);
    EXPECT_FALSE(sessions.at(0).toObject().value("cached").toBool());
    const auto secrets = doc.value(QStringLiteral("secrets")).toArray();
    ASSERT_EQ(secrets.size(), 2) << r.out;
    EXPECT_EQ(secrets.at(0).toObject().value("name").toString(), QStringLiteral("ADMIN_PASSWORD"));
    EXPECT_FALSE(secrets.at(0).toObject().value("env_set").toBool());
    EXPECT_EQ(secrets.at(1).toObject().value("name").toString(), QStringLiteral("ORDER_KEY"));
    EXPECT_TRUE(secrets.at(1).toObject().value("env_set").toBool());
    const auto hosts = doc.value(QStringLiteral("hosts")).toArray();
    ASSERT_EQ(hosts.size(), 1) << r.out;
    EXPECT_EQ(hosts.at(0).toObject().value("host").toString(),
              QStringLiteral("https://api.shop.test:443"));
    EXPECT_EQ(hosts.at(0).toObject().value("status").toString(), QStringLiteral("not_approved"));
    EXPECT_FALSE(doc.value(QStringLiteral("ready")).toBool());
    EXPECT_TRUE(doc.value(QStringLiteral("next_action"))
                    .toString()
                    .contains(QStringLiteral("do not approve")));
    EXPECT_FALSE(doc.value(QStringLiteral("next_action"))
                     .toString()
                     .contains(QStringLiteral("api.shop.test")))
        << "next_action points at fields, it does not repeat project text";
}

TEST(PlanCommand, never_writes_the_approval_file) {
    const ShopProject project;
    const auto trust = reqloom::engine::defaultProjectTrustStorePath();
    ASSERT_TRUE(trust.has_value());
    const bool existed = std::filesystem::exists(*trust);
    const auto before = existed ? reqloom::cli::tests::readText(*trust) : std::string{};
    EXPECT_EQ(plan({"order.pay", "--project", project.str()}).rc, 0);
    EXPECT_EQ(std::filesystem::exists(*trust), existed);
    if (existed) {
        EXPECT_EQ(reqloom::cli::tests::readText(*trust), before);
    }
}

TEST(PlanCommand, approved_for_the_run_by_the_environment_variable) {
    const ShopProject project;
    qputenv("REQLOOM_APPROVED_HOSTS", "api.shop.test");
    const auto r = plan({"order.pay", "--project", project.str(), "--format", "json"});
    qunsetenv("REQLOOM_APPROVED_HOSTS");
    const auto hosts = parseJson(r.out).value(QStringLiteral("hosts")).toArray();
    ASSERT_EQ(hosts.size(), 1) << r.out;
    EXPECT_EQ(hosts.at(0).toObject().value("status").toString(),
              QStringLiteral("approved_for_run"));
}

TEST(PlanCommand, text_says_nothing_is_sent_and_names_secrets_only) {
    const ShopProject project;
    const auto r = plan({"order.pay", "--project", project.str()});
    EXPECT_EQ(r.rc, 0) << r.err;
    EXPECT_NE(r.out.find("nothing is sent"), std::string::npos) << r.out;
    EXPECT_NE(r.out.find("ADMIN_PASSWORD"), std::string::npos) << r.out;
    EXPECT_NE(r.out.find("not_approved"), std::string::npos) << r.out;
}

TEST(PlanCommand, unknown_environment_is_an_invocation_error) {
    const ShopProject project;
    const auto r = plan({"order.pay", "--project", project.str(), "--env", "prod"});
    EXPECT_EQ(r.rc, 2);
}

TEST(PlanCommand, text_escapes_control_characters_in_the_operation_name) {
    TempDir project;
    project.write("reqloom.yaml",
                  "version: 1\nname: P\nresources:\n  item:\n    operations:\n"
                  "      \"re\\e[2Kad\":\n        path: /x\n");
    const auto r = plan({"item.re\x1b[2Kad", "--project", project.path().string()});
    EXPECT_EQ(r.rc, 0) << r.err;
    EXPECT_EQ(r.out.find('\x1b'), std::string::npos) << r.out;
    EXPECT_NE(r.out.find("Next: Run `reqloom run item.re\\x1b[2Kad`"), std::string::npos) << r.out;
}
