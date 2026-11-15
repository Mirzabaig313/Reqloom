// `reqloom run` tests: argument parsing, exit codes, machine-readable documents for every outcome
// and the `--output` file contract.

#include "CliTestSupport.h"

#include "commands/ByteSizeArgument.h"
#include "commands/RunCommand.h"

#include <gtest/gtest.h>

#include <QtCore/QByteArray>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QXmlStreamReader>

#include <cstddef>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace cli = reqloom::cli;
namespace fs = std::filesystem;
using reqloom::cli::tests::argList;
using reqloom::cli::tests::CapturedRun;
using reqloom::cli::tests::captureRun;
using reqloom::cli::tests::readText;
using reqloom::cli::tests::TempDir;

namespace {

// Port 1 refuses connections, so `item.read` fails at its first step without a server.
constexpr const char* kProjectYaml = R"YAML(version: 1
name: CliRun
default_environment: local
imports:
  - environments/*.yaml
environment:
  baseUrl: http://127.0.0.1:1
resources:
  item:
    operations:
      read:
        method: POST
        path: /items
        expect_status: 200
        retry:
          max: 1
)YAML";

constexpr const char* kBrokenYaml = "version: 99\nname: Broken\n";

/// A project directory holding `yaml` as its reqloom.yaml.
class ProjectDir : public TempDir {
public:
    explicit ProjectDir(const char* yaml = kProjectYaml) {
        write("reqloom.yaml", yaml);
        write("environments/staging.yaml", "baseUrl: http://127.0.0.1:1\n");
    }
    [[nodiscard]] std::string str() const { return path().string(); }
};

[[nodiscard]] CapturedRun run(const std::vector<std::string>& args) {
    return captureRun([&] { return cli::runCommand(argList(args)); });
}

[[nodiscard]] QJsonObject parseJson(const std::string& text) {
    QJsonParseError error{};
    const auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(text), &error);
    EXPECT_EQ(error.error, QJsonParseError::NoError) << text;
    EXPECT_TRUE(doc.isObject()) << text;
    return doc.object();
}

[[nodiscard]] bool wellFormedXml(const std::string& text) {
    QXmlStreamReader reader(QByteArray::fromStdString(text));
    while (!reader.atEnd()) {
        reader.readNext();
    }
    return !reader.hasError();
}

void expectErrorDocument(const std::string& out, const char* code) {
    const auto doc = parseJson(out);
    EXPECT_EQ(doc.value(QStringLiteral("schema_version")).toInt(), 1) << out;
    EXPECT_EQ(doc.value(QStringLiteral("outcome")).toString(), QStringLiteral("ERROR")) << out;
    EXPECT_TRUE(doc.value(QStringLiteral("steps")).toArray().isEmpty()) << out;
    const auto error = doc.value(QStringLiteral("error")).toObject();
    EXPECT_EQ(error.value(QStringLiteral("code")).toString(), QString::fromUtf8(code)) << out;
    EXPECT_FALSE(error.value(QStringLiteral("detail")).toString().isEmpty()) << out;
}

/// Exit code of `reqloom run` against a project that does not validate: 1 once the arguments
/// parse, 2 when they do not.
[[nodiscard]] int runWithSize(const std::string& size) {
    const ProjectDir broken{kBrokenYaml};
    return run({"item.read", "--project", broken.str(), "--failure-body-size", size}).rc;
}

}  // namespace

TEST(ByteSizeArgument, accepts_bytes_kib_and_mib) {
    EXPECT_EQ(cli::parseByteSizeArgument("0"), std::size_t{0});
    EXPECT_EQ(cli::parseByteSizeArgument("4096"), std::size_t{4096});
    EXPECT_EQ(cli::parseByteSizeArgument("16KiB"), std::size_t{16} * 1024);
    EXPECT_EQ(cli::parseByteSizeArgument("2MiB"), std::size_t{2} * 1024 * 1024);
}

TEST(ByteSizeArgument, rejects_other_text) {
    for (const char* bad : {"", "abc", "-1", "4 KiB", "4kb", "4GiB", "1.5MiB", "KiB", "12x"}) {
        EXPECT_FALSE(cli::parseByteSizeArgument(bad).has_value()) << bad;
    }
    const auto tooLarge = std::to_string(std::numeric_limits<std::size_t>::max()) + "MiB";
    EXPECT_FALSE(cli::parseByteSizeArgument(tooLarge).has_value());
}

TEST(RunCommandArgs, failure_body_size_accepts_valid_values) {
    EXPECT_EQ(runWithSize("16KiB"), 1);
    EXPECT_EQ(runWithSize("0"), 1);
    EXPECT_EQ(runWithSize("8MiB"), 1) << "above the bound is clamped, not refused";
}

TEST(RunCommandArgs, invalid_failure_body_size_is_an_argument_error) {
    EXPECT_EQ(runWithSize("lots"), 2);
    EXPECT_EQ(runWithSize("-5"), 2);
    EXPECT_EQ(run({"item.read", "--failure-body-size"}).rc, 2)
        << "a missing value is an argument error";
}

TEST(RunCommandArgs, flags_may_come_before_the_operation) {
    const ProjectDir project;
    const auto r = run({"--format", "json", "--project", project.str(), "item.read"});
    EXPECT_EQ(r.rc, 1) << r.err;
    EXPECT_EQ(parseJson(r.out).value(QStringLiteral("outcome")).toString(),
              QStringLiteral("FAILED"));
}

TEST(RunCommandArgs, equals_form_is_accepted) {
    const ProjectDir project;
    const auto r = run({"--project=" + project.str(), "item.read", "--format=json"});
    EXPECT_EQ(r.rc, 1) << r.err;
    EXPECT_EQ(parseJson(r.out).value(QStringLiteral("target")).toString(),
              QStringLiteral("item.read"));
}

TEST(RunCommandArgs, flag_without_value_says_the_value_is_missing) {
    const auto r = run({"item.read", "--env"});
    EXPECT_EQ(r.rc, 2);
    EXPECT_NE(r.err.find("--env needs a value"), std::string::npos) << r.err;
}

TEST(RunCommandArgs, value_starting_with_dashes_is_a_missing_value) {
    const ProjectDir project;
    const auto r = run({"item.read", "--project", project.str(), "--output", "--quiet"});
    EXPECT_EQ(r.rc, 2);
    EXPECT_NE(r.err.find("--output needs a value"), std::string::npos) << r.err;
    EXPECT_FALSE(fs::exists("--quiet"));
}

TEST(RunCommandArgs, double_dash_ends_flag_parsing) {
    const ProjectDir project;
    EXPECT_EQ(run({"--project", project.str(), "--format", "json", "--", "item.read"}).rc, 1);
}

TEST(RunCommandArgs, var_needs_a_non_empty_key) {
    const ProjectDir project;
    const auto r = run({"item.read", "--project", project.str(), "--var", "=x"});
    EXPECT_EQ(r.rc, 2);
    EXPECT_NE(r.err.find("--var"), std::string::npos) << r.err;
}

TEST(RunCommandArgs, repeated_single_valued_flag_is_rejected) {
    const ProjectDir project;
    const auto r =
        run({"item.read", "--project", project.str(), "--env", "local", "--env", "staging"});
    EXPECT_EQ(r.rc, 2);
    EXPECT_NE(r.err.find("more than once"), std::string::npos) << r.err;
}

TEST(RunCommandArgs, second_operation_is_rejected) {
    const ProjectDir project;
    EXPECT_EQ(run({"item.read", "item.other", "--project", project.str()}).rc, 2);
}

TEST(RunCommandArgs, help_anywhere_prints_help_and_exits_zero) {
    for (const auto& args : std::vector<std::vector<std::string>>{
             {"--help"}, {"item.read", "-h"}, {"item.read", "--bogus", "--help"}}) {
        const auto r = run(args);
        EXPECT_EQ(r.rc, 0);
        EXPECT_NE(r.out.find("Exit codes"), std::string::npos) << r.out;
    }
}

TEST(RunCommandExit, missing_project_is_an_invocation_error) {
    const TempDir empty;
    EXPECT_EQ(run({"item.read", "--project", empty.path().string()}).rc, 2);
}

TEST(RunCommandExit, unknown_operation_is_an_invocation_error) {
    const ProjectDir project;
    EXPECT_EQ(run({"item.nope", "--project", project.str()}).rc, 2);
    EXPECT_EQ(run({"nothing", "--project", project.str()}).rc, 2);
}

TEST(RunCommandExit, unknown_environment_is_an_invocation_error_listing_the_environments) {
    const ProjectDir project;
    const auto r =
        run({"item.read", "--project", project.str(), "--env", "stagng", "--var", "a=1"});
    EXPECT_EQ(r.rc, 2);
    EXPECT_NE(r.err.find("local, staging"), std::string::npos) << r.err;
}

TEST(RunCommandExit, schema_error_and_failed_step_are_run_failures) {
    const ProjectDir broken{kBrokenYaml};
    EXPECT_EQ(run({"item.read", "--project", broken.str()}).rc, 1);
    const ProjectDir project;
    EXPECT_EQ(run({"item.read", "--project", project.str(), "--quiet"}).rc, 1);
}

TEST(RunCommandMachineOutput, json_run_carries_schema_version_and_default_environment) {
    const ProjectDir project;
    const auto r = run({"item.read", "--project", project.str(), "--format", "json"});
    const auto doc = parseJson(r.out);
    EXPECT_EQ(doc.value(QStringLiteral("schema_version")).toInt(), 1);
    EXPECT_TRUE(doc.value(QStringLiteral("error")).isNull());
    EXPECT_EQ(doc.value(QStringLiteral("environment")).toString(), QStringLiteral("local"));
}

TEST(RunCommandMachineOutput, json_error_document_for_every_pre_run_failure) {
    const TempDir empty;
    const ProjectDir broken{kBrokenYaml};
    const ProjectDir project;

    auto r = run({"item.read", "--format", "json", "--project", empty.path().string()});
    EXPECT_EQ(r.rc, 2);
    expectErrorDocument(r.out, "E_CLI_PROJECT_NOT_FOUND");
    EXPECT_FALSE(r.err.empty());

    r = run({"item.read", "--format", "json", "--project", broken.str()});
    EXPECT_EQ(r.rc, 1);
    expectErrorDocument(r.out, "E_SCHEMA_VERSION");

    r = run({"item.nope", "--format", "json", "--project", project.str()});
    EXPECT_EQ(r.rc, 2);
    expectErrorDocument(r.out, "E_CLI_UNKNOWN_OPERATION");

    r = run({"item.read", "--format", "json", "--project", project.str(), "--env", "x"});
    EXPECT_EQ(r.rc, 2);
    expectErrorDocument(r.out, "E_CLI_UNKNOWN_ENVIRONMENT");

    r = run({"item.read", "--format", "json", "--bogus"});
    EXPECT_EQ(r.rc, 2);
    expectErrorDocument(r.out, "E_CLI_USAGE");
}

TEST(RunCommandMachineOutput, junit_error_document_for_unknown_environment) {
    const ProjectDir project;
    const auto r =
        run({"item.read", "--format", "junit", "--project", project.str(), "--env", "stagng"});
    EXPECT_EQ(r.rc, 2);
    EXPECT_TRUE(wellFormedXml(r.out)) << r.out;
    EXPECT_NE(r.out.find("<error type=\"E_CLI_UNKNOWN_ENVIRONMENT\""), std::string::npos) << r.out;
    EXPECT_NE(r.out.find("tests=\"1\""), std::string::npos) << r.out;
    EXPECT_NE(r.out.find("errors=\"1\""), std::string::npos) << r.out;
}

TEST(RunCommandOutputFile, text_failure_before_the_run_keeps_the_previous_report) {
    const ProjectDir project;
    const auto previous = project.write("report.txt", "previous");
    const auto r = run({"item.nope", "--project", project.str(), "--output", previous.string()});
    EXPECT_EQ(r.rc, 2);
    EXPECT_EQ(readText(previous), "previous");
}

TEST(RunCommandOutputFile, json_error_document_replaces_the_previous_report) {
    const ProjectDir project;
    const auto previous = project.write("report.json", "previous");
    const auto r = run({"item.nope",
                        "--project",
                        project.str(),
                        "--format",
                        "json",
                        "--output",
                        previous.string()});
    EXPECT_EQ(r.rc, 2);
    EXPECT_TRUE(r.out.empty()) << r.out;
    expectErrorDocument(readText(previous), "E_CLI_UNKNOWN_OPERATION");
}

TEST(RunCommandOutputFile, run_report_goes_to_the_file_and_nothing_to_stdout) {
    const ProjectDir project;
    const auto target = project.path() / "report.xml";
    const auto r = run(
        {"item.read", "--project", project.str(), "--format=junit", "--output", target.string()});
    EXPECT_EQ(r.rc, 1);
    EXPECT_TRUE(r.out.empty()) << r.out;
    EXPECT_TRUE(wellFormedXml(readText(target)));
}

TEST(RunCommandOutputFile, output_that_cannot_be_created_is_a_system_error) {
    const ProjectDir project;
    const auto target = project.path() / "no-such-dir" / "report.json";
    const auto r = run(
        {"item.read", "--project", project.str(), "--format", "json", "--output", target.string()});
    EXPECT_EQ(r.rc, 3);
    EXPECT_NE(r.err.find("report.json"), std::string::npos) << r.err;
}

TEST(RunCommandOutputFile, unwritable_directory_keeps_the_previous_report) {
    const ProjectDir project;
    const TempDir outDir;
    const auto previous = outDir.write("report.json", "previous");
    fs::permissions(outDir.path(), fs::perms::owner_read | fs::perms::owner_exec);
    if (fs::path probe = outDir.path() / "probe"; std::ofstream{probe}.good()) {
        fs::remove(probe);
        GTEST_SKIP() << "running with permission to write into a read-only directory";
    }
    const auto r = run({"item.read",
                        "--project",
                        project.str(),
                        "--format",
                        "json",
                        "--output",
                        previous.string()});
    EXPECT_EQ(r.rc, 3);
    EXPECT_EQ(readText(previous), "previous");
}
