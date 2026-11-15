// `reqloom run` argument tests: `--failure-body-size` parsing and its exit codes.

#include "commands/ByteSizeArgument.h"
#include "commands/RunCommand.h"

#include <gtest/gtest.h>

#include <QtCore/QString>
#include <QtCore/QStringList>

#include <cstddef>
#include <limits>
#include <string>

namespace cli = reqloom::cli;

namespace {

/// Exit code of `reqloom run` with a project path that holds no reqloom.yaml: 1 once the
/// arguments parse, 2 when they do not.
[[nodiscard]] int runWithSize(const QString& size) {
    return cli::runCommand(QStringList{QStringLiteral("item.read"),
                                       QStringLiteral("--project"),
                                       QStringLiteral("/nonexistent/reqloom-run-test"),
                                       QStringLiteral("--failure-body-size"),
                                       size});
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
    EXPECT_EQ(runWithSize(QStringLiteral("16KiB")), 1);
    EXPECT_EQ(runWithSize(QStringLiteral("0")), 1);
    EXPECT_EQ(runWithSize(QStringLiteral("8MiB")), 1) << "above the bound is clamped, not refused";
}

TEST(RunCommandArgs, invalid_failure_body_size_is_an_argument_error) {
    EXPECT_EQ(runWithSize(QStringLiteral("lots")), 2);
    EXPECT_EQ(runWithSize(QStringLiteral("-5")), 2);
    EXPECT_EQ(cli::runCommand(
                  QStringList{QStringLiteral("item.read"), QStringLiteral("--failure-body-size")}),
              2)
        << "a missing value is an argument error";
}
