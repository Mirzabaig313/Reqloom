// Command-line option parsing shared by `reqloom run`, `lint` and `import`.
#pragma once

#include <QtCore/QStringList>

#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace reqloom::cli {

/// One `--name` option a command accepts.
struct OptionSpec {
    std::string_view name;
    bool takesValue{false};
    /// Only for options that take a value; a flag may always be repeated.
    bool repeatable{false};
};

/// Options and positional arguments, each as UTF-8.
struct ParsedArguments {
    bool help{false};
    std::vector<std::string> positionals;
    std::map<std::string, std::vector<std::string>, std::less<>> options;

    [[nodiscard]] bool has(std::string_view name) const { return options.contains(name); }

    /// The value of a single-valued option, or nullopt when it was not given.
    [[nodiscard]] std::optional<std::string> value(std::string_view name) const;

    /// Every value of a repeatable option, in the order given.
    [[nodiscard]] std::vector<std::string> values(std::string_view name) const;
};

/// Parse `args` (without the command name) against `specs`.
///
/// Options and positionals may come in any order; `--name value` and `--name=value` both work.
/// A separate value that starts with `--` counts as missing, so it must be written `--name=--x`.
/// `--` ends option parsing. `--help` or `-h` before `--` sets `help` and wins over every error.
/// @return The parsed arguments, or a one-line message naming the offending argument.
[[nodiscard]] std::expected<ParsedArguments, std::string> parseArguments(
    const QStringList& args, std::span<const OptionSpec> specs);

/// The value of `--format` in `args` when it is one of `allowed`, without parsing anything else,
/// so an invocation error can still be reported in the format the caller asked for.
[[nodiscard]] std::optional<std::string> peekFormat(const QStringList& args,
                                                    std::span<const std::string_view> allowed);

}  // namespace reqloom::cli
