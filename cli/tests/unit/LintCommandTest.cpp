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

// ─── FR-15.5: errors and warnings say where, what, and what would satisfy it ─────────────────

TEST(LintCommand, json_error_carries_file_line_reference_fix_and_next_action) {
    const ProjectDir project{
        "version: 1\n"              // 1
        "name: P\n"                 // 2
        "actors:\n"                 // 3
        "  admin:\n"                // 4
        "    description: x\n"      // 5
        "resources:\n"              // 6
        "  item:\n"                 // 7
        "    operations:\n"         // 8
        "      read:\n"             // 9
        "        method: GET\n"     // 10
        "        path: /x\n"        // 11
        "        actor: amdin\n"};  // 12
    const auto r = lint({project.str(), "--format", "json"});
    EXPECT_EQ(r.rc, 1);
    const auto doc = parseJson(r.out);
    EXPECT_EQ(doc.value(QStringLiteral("contract_version")).toString(), QStringLiteral("1.2"));
    const auto errors = doc.value(QStringLiteral("errors")).toArray();
    ASSERT_EQ(errors.size(), 1) << r.out;
    const auto e = errors.at(0).toObject();
    EXPECT_EQ(e.value(QStringLiteral("code")).toString(), QStringLiteral("E_REF_UNDEFINED"));
    EXPECT_EQ(e.value(QStringLiteral("file")).toString(), QStringLiteral("reqloom.yaml")) << r.out;
    EXPECT_EQ(e.value(QStringLiteral("line")).toInt(), 12) << r.out;
    EXPECT_EQ(e.value(QStringLiteral("reference")).toString(), QStringLiteral("amdin"));
    EXPECT_TRUE(e.value(QStringLiteral("fix")).toString().contains(QStringLiteral("admin")));
    EXPECT_FALSE(e.value(QStringLiteral("next_action")).toString().isEmpty());
    // The original fields keep their meaning.
    EXPECT_TRUE(e.value(QStringLiteral("detail")).toString().contains(QStringLiteral("amdin")));
}

TEST(LintCommand, text_error_names_file_line_reference_and_fix) {
    const ProjectDir project{
        "version: 1\nname: P\nresources:\n  item:\n    operations:\n"
        "      read:\n        method: GET\n        path: /x/{{ghost.id}}\n"};
    const auto r = lint({project.str()});
    EXPECT_EQ(r.rc, 1);
    EXPECT_NE(r.err.find("LINT FAIL [E_REF_UNDEFINED]: "), std::string::npos) << r.err;
    EXPECT_NE(r.err.find("reqloom.yaml:8"), std::string::npos) << r.err;
    EXPECT_NE(r.err.find("{{ghost.id}}"), std::string::npos) << r.err;
    EXPECT_NE(r.err.find("fix:"), std::string::npos) << r.err;
}

TEST(LintCommand, warns_about_a_reference_no_operation_extracts) {
    const ProjectDir project{
        "version: 1\nname: P\nresources:\n  item:\n    operations:\n"
        "      create:\n        method: POST\n        path: /x\n"
        "        extract:\n          item_id: $.id\n"
        "      read:\n        method: GET\n"
        "        path: /x/{{item.itme_id}}\n"};  // 13
    const auto r = lint({project.str(), "--format", "json"});
    EXPECT_EQ(r.rc, 0) << r.err;
    const auto doc = parseJson(r.out);
    const auto warnings = doc.value(QStringLiteral("warnings")).toArray();
    ASSERT_EQ(warnings.size(), 1) << r.out;
    EXPECT_TRUE(warnings.at(0).toString().contains(QStringLiteral("itme_id"))) << r.out;
    const auto details = doc.value(QStringLiteral("warning_details")).toArray();
    ASSERT_EQ(details.size(), 1) << r.out;
    const auto w = details.at(0).toObject();
    EXPECT_EQ(w.value(QStringLiteral("file")).toString(), QStringLiteral("reqloom.yaml"));
    EXPECT_EQ(w.value(QStringLiteral("line")).toInt(), 13);
    EXPECT_EQ(w.value(QStringLiteral("reference")).toString(), QStringLiteral("{{item.itme_id}}"));
    EXPECT_TRUE(w.value(QStringLiteral("fix")).toString().contains(QStringLiteral("item_id")));
}

TEST(LintCommand, warning_details_give_the_position_of_load_warnings) {
    const ProjectDir project{
        "version: 1\nname: W\nmystery_key: 1\nresources:\n  item:\n    operations:\n"
        "      read:\n        method: GET\n        path: /x\n"};
    const auto r = lint({project.str(), "--format", "json"});
    const auto details = parseJson(r.out).value(QStringLiteral("warning_details")).toArray();
    ASSERT_EQ(details.size(), 1) << r.out;
    const auto w = details.at(0).toObject();
    EXPECT_EQ(w.value(QStringLiteral("line")).toInt(), 3);
    EXPECT_EQ(w.value(QStringLiteral("column")).toInt(), 1);
    EXPECT_TRUE(
        w.value(QStringLiteral("message")).toString().contains(QStringLiteral("mystery_key")));
}

TEST(LintCommand, text_output_escapes_terminal_control_characters_from_the_project) {
    const ProjectDir project{
        "version: 1\nname: P\nresources:\n  item:\n    operations:\n"
        "      read:\n        method: GET\n"
        "        path: \"/x/{{gh\\e[2Kost.id}}\"\n"};
    const auto r = lint({project.str()});
    EXPECT_EQ(r.err.find('\x1b'), std::string::npos) << r.err;
}
