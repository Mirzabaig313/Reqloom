// Tests for the argument parser shared by `run`, `lint` and `import`, and the path conversions.

#include "commands/ArgParser.h"
#include "commands/PathText.h"

#include <gtest/gtest.h>

#include <QtCore/QString>
#include <QtCore/QStringList>

#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace cli = reqloom::cli;

namespace {

constexpr std::array kSpecs{
    cli::OptionSpec{.name = "--env", .takesValue = true},
    cli::OptionSpec{.name = "--var", .takesValue = true, .repeatable = true},
    cli::OptionSpec{.name = "--quiet"},
};

[[nodiscard]] auto parse(const std::vector<std::string>& parts) {
    QStringList args;
    for (const auto& part : parts) {
        args.append(QString::fromStdString(part));
    }
    return cli::parseArguments(args, kSpecs);
}

}  // namespace

TEST(ArgParser, options_and_positionals_in_any_order) {
    const auto parsed = parse({"--env", "dev", "op.one", "--quiet", "--var", "a=1", "--var=b=2"});
    ASSERT_TRUE(parsed) << parsed.error();
    EXPECT_EQ(parsed->positionals, std::vector<std::string>{"op.one"});
    EXPECT_EQ(parsed->value("--env"), "dev");
    EXPECT_TRUE(parsed->has("--quiet"));
    EXPECT_EQ(parsed->values("--var"), (std::vector<std::string>{"a=1", "b=2"}));
    EXPECT_FALSE(parsed->help);
}

TEST(ArgParser, equals_form_keeps_everything_after_the_first_equals) {
    const auto parsed = parse({"--env=a=b=c"});
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->value("--env"), "a=b=c");
}

TEST(ArgParser, equals_form_accepts_a_value_starting_with_dashes) {
    const auto parsed = parse({"--env=--odd"});
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->value("--env"), "--odd");
}

TEST(ArgParser, missing_value_is_reported_as_missing) {
    for (const auto& args :
         std::vector<std::vector<std::string>>{{"--env"}, {"--env", "--quiet"}, {"--env="}}) {
        const auto parsed = parse(args);
        ASSERT_FALSE(parsed);
        EXPECT_NE(parsed.error().find("--env needs a value"), std::string::npos) << parsed.error();
    }
}

TEST(ArgParser, unknown_and_malformed_options_are_rejected) {
    EXPECT_NE(parse({"--nope"}).error().find("unknown option '--nope'"), std::string::npos);
    EXPECT_NE(parse({"-x"}).error().find("unknown option '-x'"), std::string::npos);
    EXPECT_NE(parse({"--quiet=yes"}).error().find("--quiet takes no value"), std::string::npos);
}

TEST(ArgParser, repeated_single_valued_option_is_rejected) {
    const auto parsed = parse({"--env", "a", "--env=b"});
    ASSERT_FALSE(parsed);
    EXPECT_NE(parsed.error().find("--env given more than once"), std::string::npos);
}

TEST(ArgParser, double_dash_ends_options) {
    const auto parsed = parse({"--quiet", "--", "--env", "-h", "-"});
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->positionals, (std::vector<std::string>{"--env", "-h", "-"}));
    EXPECT_FALSE(parsed->help);
    EXPECT_FALSE(parsed->has("--env"));
}

TEST(ArgParser, help_anywhere_wins_over_other_errors) {
    for (const auto& args : std::vector<std::vector<std::string>>{
             {"--help"}, {"op", "-h"}, {"--nope", "--help"}, {"--env", "--help"}}) {
        const auto parsed = parse(args);
        ASSERT_TRUE(parsed);
        EXPECT_TRUE(parsed->help);
    }
}

TEST(PathText, round_trips_non_ascii_paths_through_utf8) {
    const std::string utf8 = "proj/\xC3\xA9t\xC3\xA9/\xE6\x97\xA5\xE6\x9C\xAC";
    const auto path = cli::pathFromUtf8(utf8);
    EXPECT_TRUE(path.u8string() ==
                std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
    EXPECT_EQ(cli::pathToUtf8(path), utf8);
}
