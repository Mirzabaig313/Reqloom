// Command-line option parsing shared by `reqloom run`, `lint` and `import`.

#include "ArgParser.h"

#include <QtCore/QString>

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace reqloom::cli {

namespace {

constexpr std::string_view kEndOfOptions = "--";

[[nodiscard]] bool isHelp(const std::string& token) {
    return token == "--help" || token == "-h";
}

[[nodiscard]] bool looksLikeOption(const std::string& token) {
    return token.size() > 1 && token.front() == '-';
}

[[nodiscard]] std::vector<std::string> toUtf8(const QStringList& args) {
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(args.size()));
    for (const auto& arg : args) {
        out.push_back(arg.toStdString());
    }
    return out;
}

[[nodiscard]] const OptionSpec* findSpec(std::span<const OptionSpec> specs, std::string_view name) {
    const auto it = std::ranges::find(specs, name, &OptionSpec::name);
    return it == specs.end() ? nullptr : &*it;
}

class Parser {
public:
    Parser(std::vector<std::string> tokens, std::span<const OptionSpec> specs)
        : tokens_(std::move(tokens)), specs_(specs) {}

    [[nodiscard]] std::expected<ParsedArguments, std::string> run() {
        bool optionsEnded = false;
        while (next_ < tokens_.size()) {
            const std::string token = tokens_[next_++];
            if (optionsEnded || !looksLikeOption(token)) {
                out_.positionals.push_back(token);
            } else if (token == kEndOfOptions) {
                optionsEnded = true;
            } else if (auto error = option(token)) {
                return std::unexpected(std::move(*error));
            }
        }
        return std::move(out_);
    }

private:
    [[nodiscard]] std::optional<std::string> option(const std::string& token) {
        const auto eq = token.find('=');
        const std::string name = token.substr(0, eq);
        const OptionSpec* spec = token.starts_with("--") ? findSpec(specs_, name) : nullptr;
        if (spec == nullptr) {
            return std::format("unknown option '{}'", name);
        }
        if (!spec->takesValue) {
            if (eq != std::string::npos) {
                return std::format("{} takes no value", name);
            }
            out_.options[name];
            return std::nullopt;
        }
        std::string value;
        if (eq != std::string::npos) {
            value = token.substr(eq + 1);
        } else if (next_ < tokens_.size() && !tokens_[next_].starts_with("--")) {
            value = tokens_[next_++];
        } else if (next_ < tokens_.size()) {
            return std::format(
                "{0} needs a value (write {0}=VALUE for a value that starts with --)", name);
        }
        if (value.empty()) {
            return std::format("{} needs a value", name);
        }
        auto& slot = out_.options[name];
        if (!slot.empty() && !spec->repeatable) {
            return std::format("{} given more than once", name);
        }
        slot.push_back(std::move(value));
        return std::nullopt;
    }

    std::vector<std::string> tokens_;
    std::span<const OptionSpec> specs_;
    std::size_t next_{0};
    ParsedArguments out_;
};

}  // namespace

std::optional<std::string> ParsedArguments::value(std::string_view name) const {
    const auto it = options.find(name);
    if (it == options.end() || it->second.empty()) {
        return std::nullopt;
    }
    return it->second.front();
}

std::vector<std::string> ParsedArguments::values(std::string_view name) const {
    const auto it = options.find(name);
    return it == options.end() ? std::vector<std::string>{} : it->second;
}

std::expected<ParsedArguments, std::string> parseArguments(const QStringList& args,
                                                           std::span<const OptionSpec> specs) {
    auto tokens = toUtf8(args);
    const auto end = std::ranges::find(tokens, kEndOfOptions);
    if (std::any_of(tokens.begin(), end, isHelp)) {
        ParsedArguments help;
        help.help = true;
        return help;
    }
    return Parser{std::move(tokens), specs}.run();
}

std::optional<std::string> peekFormat(const QStringList& args,
                                      std::span<const std::string_view> allowed) {
    const auto tokens = toUtf8(args);
    for (std::size_t i = 0; i < tokens.size() && tokens[i] != kEndOfOptions; ++i) {
        std::string candidate;
        if (tokens[i].starts_with("--format=")) {
            candidate = tokens[i].substr(std::string_view{"--format="}.size());
        } else if (tokens[i] == "--format" && i + 1 < tokens.size()) {
            candidate = tokens[i + 1];
        }
        if (std::ranges::find(allowed, candidate) != allowed.end()) {
            return candidate;
        }
    }
    return std::nullopt;
}

}  // namespace reqloom::cli
