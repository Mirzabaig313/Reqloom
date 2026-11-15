// Tests for writing a finished report to a file or a stream.

#include "CliTestSupport.h"

#include "output/ReportWriter.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <string>

namespace cli = reqloom::cli;
namespace fs = std::filesystem;
using reqloom::cli::tests::readText;
using reqloom::cli::tests::TempDir;

TEST(ReportWriter, file_is_replaced_by_the_complete_document) {
    const TempDir dir;
    const auto target = dir.write("report.json", "previous");
    ASSERT_TRUE(cli::writeReportFile(target, "{\"new\": true}\n"));
    EXPECT_EQ(readText(target), "{\"new\": true}\n");
    std::size_t entries = 0;
    for ([[maybe_unused]] const auto& entry : fs::directory_iterator(dir.path())) {
        ++entries;
    }
    EXPECT_EQ(entries, 1u) << "no temporary file is left behind";
}

TEST(ReportWriter, file_in_a_missing_directory_fails_with_its_name) {
    const TempDir dir;
    const auto result = cli::writeReportFile(dir.path() / "absent" / "report.json", "x");
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().find("report.json"), std::string::npos) << result.error();
}

TEST(ReportWriter, stream_write_failure_is_reported) {
    const TempDir dir;
    const auto readOnly = dir.write("read-only.txt", "");
    std::FILE* stream = std::fopen(readOnly.string().c_str(), "r");
    ASSERT_NE(stream, nullptr);
    const auto result = cli::writeReportStream(stream, "document");
    std::fclose(stream);
    EXPECT_FALSE(result);
}

TEST(ReportWriter, stream_receives_the_document) {
    const TempDir dir;
    const auto path = dir.path() / "out.txt";
    // Binary mode: a text-mode stream on Windows writes "\r\n".
    std::FILE* stream = std::fopen(path.string().c_str(), "wb");
    ASSERT_NE(stream, nullptr);
    EXPECT_TRUE(cli::writeReportStream(stream, "document\n"));
    std::fclose(stream);
    EXPECT_EQ(readText(path), "document\n");
}
