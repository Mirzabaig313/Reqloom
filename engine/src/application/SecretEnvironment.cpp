// SecretEnvironment — see SecretEnvironment.h.

#include "SecretEnvironment.h"

#include <reqloom/engine/SecretStore.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace reqloom::engine {

namespace {

[[nodiscard]] bool isMappableChar(const char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

[[nodiscard]] std::string_view trimmed(std::string_view s) noexcept {
    const auto begin = s.find_first_not_of(" \t");
    if (begin == std::string_view::npos) {
        return {};
    }
    return s.substr(begin, s.find_last_not_of(" \t") - begin + 1);
}

/// The bare reference inside `{{...}}`: the text itself, or a builtin call's unquoted argument.
[[nodiscard]] std::string_view bareReference(std::string_view inner) noexcept {
    if (!inner.starts_with('$')) {
        return inner;
    }
    const auto open = inner.find('(');
    if (open == std::string_view::npos || !inner.ends_with(')')) {
        return {};
    }
    return trimmed(inner.substr(open + 1, inner.size() - open - 2));
}

void collectFrom(std::string_view text,
                 const std::map<std::string, std::string>* envVars,
                 std::vector<std::string>& out) {
    constexpr std::string_view kSecret{"secret."};
    constexpr std::string_view kEnv{"env."};
    std::size_t pos{};
    while ((pos = text.find("{{", pos)) != std::string_view::npos) {
        const auto close = text.find("}}", pos + 2);
        if (close == std::string_view::npos) {
            return;
        }
        const auto ref = bareReference(trimmed(text.substr(pos + 2, close - pos - 2)));
        pos = close + 2;
        if (ref.starts_with(kSecret) && ref.size() > kSecret.size()) {
            std::string name{ref.substr(kSecret.size())};
            if (std::ranges::find(out, name) == out.end()) {
                out.push_back(std::move(name));
            }
        } else if (envVars != nullptr && ref.starts_with(kEnv)) {
            const auto it = envVars->find(std::string{ref.substr(kEnv.size())});
            if (it != envVars->end()) {
                // One level only: an environment value cannot loop back on itself.
                collectFrom(it->second, nullptr, out);
            }
        }
    }
}

}  // namespace

std::optional<std::string> secretEnvironmentVariable(std::string_view name) {
    if (name.empty() || !std::ranges::all_of(name, isMappableChar)) {
        return std::nullopt;
    }
    std::string variable{kSecretEnvironmentPrefix};
    for (const char c : name) {
        variable.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return variable;
}

std::optional<std::string> sharedEnvironmentVariable(const std::vector<std::string>& names) {
    std::map<std::string, std::string> firstByVariable{};
    for (const auto& name : names) {
        const auto variable = secretEnvironmentVariable(name);
        if (!variable) {
            continue;
        }
        const auto [it, inserted] = firstByVariable.try_emplace(*variable, name);
        // NOLINTNEXTLINE(concurrency-mt-unsafe) — read once at run start.
        if (!inserted && std::getenv(variable->c_str()) != nullptr) {
            return "'" + it->second + "' and '" + name + "' both map to " + *variable;
        }
    }
    return std::nullopt;
}

std::optional<std::string> readSecretFromEnvironment(const std::string& name,
                                                     SecretSourceState& state) {
    const auto variable = secretEnvironmentVariable(name);
    if (!variable) {
        return std::nullopt;
    }
    // NOLINTNEXTLINE(concurrency-mt-unsafe) — read once at run start, as `$.env` reads are.
    const char* value = std::getenv(variable->c_str());
    if (value == nullptr) {
        return std::nullopt;
    }
    if (*value == '\0') {
        // A CI system often expands a missing secret to "": say so instead of sending nothing.
        state.emptyVariables.insert(name);
        return std::nullopt;
    }
    return std::string{value};
}

std::string missingSecretCause(std::string_view name, const SecretSourceState& state) {
    const auto variable = secretEnvironmentVariable(name);
    if (!state.environmentEnabled) {
        // The desktop reads no environment: point at the keychain, or the CLI.
        if (state.backendAvailable) {
            return "the secret '" + std::string{name} +
                   "' has no value. Store it with Manage Secrets.";
        }
        return "no secret backend available; {{secret." + std::string{name} +
               "}} cannot be read here. Run on a desktop session with a keychain" +
               (variable ? ", or run the reqloom CLI with " + *variable + " set." : ".");
    }
    std::string cause{};
    if (!variable) {
        cause = "the secret '" + std::string{name} +
                "' cannot be supplied through an environment variable: only a name made of "
                "letters, digits and '_' maps to " +
                std::string{kSecretEnvironmentPrefix} + "<NAME>. ";
        cause += state.backendAvailable
                     ? "Store it with Manage Secrets in the desktop app, or rename it."
                     : "No secret backend is available here; rename the secret, or run on a "
                       "desktop session with a keychain.";
        return cause;
    }
    if (state.backendAvailable) {
        cause = "the secret '" + std::string{name} + "' has no value. Set " + *variable +
                ", or store it with Manage Secrets in the desktop app.";
    } else {
        cause = "no secret backend available; set " + *variable +
                " or run on a desktop session with a keychain.";
    }
    if (state.emptyVariables.contains(std::string{name})) {
        cause += " " + *variable + " is set but empty.";
    }
    return cause;
}

std::vector<std::string> referencedSecretNames(const std::vector<std::string_view>& templates,
                                               const std::map<std::string, std::string>& envVars) {
    std::vector<std::string> names{};
    for (const auto text : templates) {
        collectFrom(text, &envVars, names);
    }
    return names;
}

}  // namespace reqloom::engine
