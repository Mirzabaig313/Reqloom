// `reqloom lint` tests: argument parsing, exit codes and the JSON report.

#include "CliTestSupport.h"

#include "commands/LintCommand.h"

#include <gtest/gtest.h>

#include <QtCore/QByteArray>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>

#include <string>
#include <vector>

namespace cli = reqloom::cli;
using reqloom::cli::tests::argList;
using reqloom::cli::tests::CapturedRun;
using reqloom::cli::tests::captureRun;
using reqloom::cli::tests::TempDir;

namespace {

constexpr const char* kProjectYaml = R"YAML(version: 1
name: CliLint
default_environment: local
environment:
  baseUrl: http://127.0.0.1:1
resources:
  item:
    operations:
      read:
        method: GET
        path: /items
        expect_status: 200
)YAML";

class ProjectDir : public TempDir {
public:
    explicit ProjectDir(const char* yaml = kProjectYaml) { write("reqloom.yaml", yaml); }
    [[nodiscard]] std::string str() const { return path().string(); }
};

[[nodiscard]] CapturedRun lint(const std::vector<std::string>& args) {
    return captureRun([&] { return cli::lintCommand(argList(args)); });
}

[[nodiscard]] QJsonObject parseJson(const std::string& text) {
    QJsonParseError error{};
    const auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(text), &error);
    EXPECT_EQ(error.error, QJsonParseError::NoError) << text;
    return doc.object();
}

}  // namespace

TEST(LintCommand, positional_project_path_is_linted) {
    const ProjectDir project;
    const auto r = lint({project.str()});
    EXPECT_EQ(r.rc, 0) << r.err;
    EXPECT_NE(r.out.find("LINT OK"), std::string::npos) << r.out;
}

TEST(LintCommand, project_flag_is_linted) {
    const ProjectDir project;
    EXPECT_EQ(lint({"--project", project.str()}).rc, 0);
    EXPECT_EQ(lint({"--project=" + project.str()}).rc, 0);
}

TEST(LintCommand, unknown_flag_is_an_invocation_error) {
    const ProjectDir project;
    const auto r = lint({"--projet", project.str()});
    EXPECT_EQ(r.rc, 2);
    EXPECT_EQ(r.out.find("LINT OK"), std::string::npos) << r.out;
}

TEST(LintCommand, project_flag_without_value_is_an_invocation_error) {
    EXPECT_EQ(lint({"--project"}).rc, 2);
}

TEST(LintCommand, positional_and_project_flag_together_are_rejected) {
    const ProjectDir project;
    EXPECT_EQ(lint({project.str(), "--project", project.str()}).rc, 2);
    EXPECT_EQ(lint({project.str(), project.str()}).rc, 2);
}

TEST(LintCommand, help_prints_help_and_exits_zero) {
    const auto r = lint({"--help"});
    EXPECT_EQ(r.rc, 0);
    EXPECT_NE(r.out.find("Exit codes"), std::string::npos) << r.out;
}

TEST(LintCommand, missing_project_is_an_invocation_error) {
    const TempDir empty;
    EXPECT_EQ(lint({empty.path().string()}).rc, 2);
}

TEST(LintCommand, schema_error_is_a_failure) {
    const ProjectDir broken{"version: 99\n"};
    EXPECT_EQ(lint({broken.str()}).rc, 1);
}

TEST(LintCommand, json_report_for_a_valid_project) {
    const ProjectDir project;
    const auto r = lint({project.str(), "--format", "json"});
    EXPECT_EQ(r.rc, 0) << r.err;
    const auto doc = parseJson(r.out);
    EXPECT_EQ(doc.value(QStringLiteral("schema_version")).toInt(), 1);
    EXPECT_TRUE(doc.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(doc.value(QStringLiteral("errors")).toArray().isEmpty());
    EXPECT_TRUE(doc.value(QStringLiteral("warnings")).isArray());
}

TEST(LintCommand, json_report_for_a_schema_error) {
    const ProjectDir broken{"version: 99\n"};
    const auto r = lint({"--format=json", broken.str()});
    EXPECT_EQ(r.rc, 1);
    const auto doc = parseJson(r.out);
    EXPECT_FALSE(doc.value(QStringLiteral("ok")).toBool());
    const auto errors = doc.value(QStringLiteral("errors")).toArray();
    ASSERT_EQ(errors.size(), 1);
    EXPECT_EQ(errors.at(0).toObject().value(QStringLiteral("code")).toString(),
              QStringLiteral("E_SCHEMA_VERSION"));
}

TEST(LintCommand, json_report_for_a_missing_project_and_bad_arguments) {
    const TempDir empty;
    auto r = lint({empty.path().string(), "--format", "json"});
    EXPECT_EQ(r.rc, 2);
    EXPECT_FALSE(parseJson(r.out).value(QStringLiteral("ok")).toBool());

    r = lint({"--format", "json", "--bogus"});
    EXPECT_EQ(r.rc, 2);
    const auto errors = parseJson(r.out).value(QStringLiteral("errors")).toArray();
    ASSERT_EQ(errors.size(), 1);
    EXPECT_EQ(errors.at(0).toObject().value(QStringLiteral("code")).toString(),
              QStringLiteral("E_CLI_USAGE"));
}

TEST(LintCommand, json_report_warns_about_unknown_keys) {
    const ProjectDir project{
        "version: 1\nname: W\nmystery_key: 1\nresources:\n  item:\n    operations:\n"
        "      read:\n        method: GET\n        path: /x\n"};
    const auto r = lint({project.str(), "--format", "json"});
    EXPECT_EQ(r.rc, 0) << r.err;
    const auto warnings = parseJson(r.out).value(QStringLiteral("warnings")).toArray();
    ASSERT_FALSE(warnings.isEmpty()) << r.out;
    EXPECT_TRUE(warnings.at(0).toString().contains(QStringLiteral("mystery_key"))) << r.out;
}

TEST(LintCommand, warns_about_a_secret_name_no_environment_variable_can_supply) {
    const ProjectDir project{
        "version: 1\nname: S\nid: lint-secret-names\nresources:\n  item:\n    operations:\n"
        "      read:\n        method: GET\n        path: /x\n        headers:\n"
        "          X-One: \"{{secret.api-key}}\"\n          X-Two: \"{{secret.GOOD_NAME}}\"\n"};
    const auto r = lint({project.str(), "--format", "json"});
    EXPECT_EQ(r.rc, 0) << r.err;
    const auto warnings = parseJson(r.out).value(QStringLiteral("warnings")).toArray();
    ASSERT_EQ(warnings.size(), 1) << r.out;
    const auto text = warnings.at(0).toString();
    EXPECT_TRUE(text.contains(QStringLiteral("api-key"))) << r.out;
    EXPECT_TRUE(text.contains(QStringLiteral("REQLOOM_SECRET_"))) << r.out;
}

TEST(LintCommand, flags_two_secret_names_that_share_one_environment_variable) {
    const ProjectDir project{
        "version: 1\nname: S\nid: lint-secret-share\nresources:\n  item:\n    operations:\n"
        "      read:\n        method: GET\n        path: /x\n        headers:\n"
        "          X-One: \"{{secret.api_key}}\"\n          X-Two: \"{{secret.API_KEY}}\"\n"};
    const auto r = lint({project.str(), "--format", "json"});
    EXPECT_EQ(r.rc, 0) << r.err;
    const auto warnings = parseJson(r.out).value(QStringLiteral("warnings")).toArray();
    ASSERT_EQ(warnings.size(), 1) << r.out;
    const auto text = warnings.at(0).toString();
    EXPECT_TRUE(text.contains(QStringLiteral("REQLOOM_SECRET_API_KEY"))) << r.out;
    EXPECT_TRUE(text.contains(QStringLiteral("api_key"))) << r.out;
}

TEST(LintCommand, missing_id_warning_says_environment_secrets_still_work) {
    const ProjectDir project{
        "version: 1\nname: S\nresources:\n  item:\n    operations:\n"
        "      read:\n        method: GET\n        path: /x\n        headers:\n"
        "          X-One: \"{{secret.API_KEY}}\"\n"};
    const auto r = lint({project.str(), "--format", "json"});
    const auto warnings = parseJson(r.out).value(QStringLiteral("warnings")).toArray();
    ASSERT_EQ(warnings.size(), 1) << r.out;
    const auto text = warnings.at(0).toString();
    EXPECT_FALSE(text.contains(QStringLiteral("runs will fail"))) << r.out;
    EXPECT_TRUE(text.contains(QStringLiteral("keychain"))) << r.out;
    EXPECT_TRUE(text.contains(QStringLiteral("REQLOOM_SECRET_"))) << r.out;
}
