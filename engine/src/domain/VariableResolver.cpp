// VariableResolver — substitutes {{X.y}} references per PRD §5.7 and
// Engine Req §3.10.
//
// Resolution order (first match wins — AC-3.10.1):
//   1. Builtins ($.now, $.uuid, $.faker.*, $.env.*)
//   2. Actor sessions
//   3. Resource extractions (most-recent or indexed)
//   4. Environment variables (env.X)
//   5. Secrets (secret.X)
#include "VariableResolver.h"

#include <charconv>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <random>
#include <regex>
#include <sstream>
#include <string>

namespace chainapi::engine {

namespace {

/// Generate a UUID v4 string. RNG is thread_local to avoid races on the
/// shared static; this is fine for both the engine's sequential use and
/// any future multi-engine setups.
std::string generateUuid() {
    thread_local std::mt19937_64 gen{std::random_device{}()};
    std::uniform_int_distribution<std::uint32_t> dist;

    auto r = [&]() { return dist(gen); };
    auto hex = [](std::uint32_t v, int digits) {
        // Mask to the requested width without invoking 1u<<32 UB.
        const std::uint64_t mask = (digits >= 8)
            ? std::uint64_t{0xFFFF'FFFFu}
            : ((std::uint64_t{1} << (digits * 4)) - 1);
        std::ostringstream ss;
        ss << std::hex << std::setfill('0') << std::setw(digits)
           << (static_cast<std::uint64_t>(v) & mask);
        return ss.str();
    };

    return hex(r(), 8) + "-" + hex(r(), 4) + "-4" + hex(r(), 3) +
           "-" + hex(0x8 | (r() & 0x3), 1) + hex(r(), 3) + "-" +
           hex(r(), 8) + hex(r(), 4);
}

/// ISO 8601 current timestamp (thread-safe via gmtime_r / gmtime_s).
std::string nowIso() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm buf{};
#ifdef _WIN32
    gmtime_s(&buf, &time);
#else
    gmtime_r(&time, &buf);
#endif
    std::ostringstream ss;
    ss << std::put_time(&buf, "%FT%TZ");
    return ss.str();
}

}  // namespace

VariableResolver::VariableResolver() = default;
VariableResolver::~VariableResolver() = default;

VariableResolver::Result VariableResolver::resolve(
    std::string_view templateStr,
    const RunContext& ctx,
    const ResolveContext& resolveCtx) const {

    static const std::regex refPattern(R"(\{\{([^}]+)\}\})");
    std::string input(templateStr);
    std::string output;
    std::vector<std::string> unresolved;

    std::sregex_iterator begin(input.begin(), input.end(), refPattern);
    std::sregex_iterator end;
    std::size_t lastPos = 0;

    for (auto it = begin; it != end; ++it) {
        auto matchPos = static_cast<std::size_t>(it->position());
        output += input.substr(lastPos, matchPos - lastPos);

        auto ref = (*it)[1].str();
        // Trim leading/trailing whitespace so `{{ user.id }}` works the
        // same as `{{user.id}}`.
        auto refStart = ref.find_first_not_of(" \t");
        auto refEnd = ref.find_last_not_of(" \t");
        if (refStart != std::string::npos) {
            ref = ref.substr(refStart, refEnd - refStart + 1);
        }
        std::optional<std::string> resolved;

        // 1. Builtins
        if (ref == "$.uuid") {
            resolved = generateUuid();
        } else if (ref == "$.now") {
            resolved = nowIso();
        } else if (ref.starts_with("$.env.")) {
            // OS environment variable
            auto envName = ref.substr(6);
            if (auto* val = std::getenv(envName.c_str())) {
                resolved = std::string(val);
            }
        } else if (ref.starts_with("$.faker.")) {
            // Simple faker: just generate unique values
            auto fakerType = ref.substr(8);
            if (fakerType == "email") {
                resolved = "test+" + generateUuid().substr(0, 8) + "@example.com";
            } else if (fakerType == "phone") {
                resolved = "+1555" + std::to_string(std::random_device{}() % 10000000);
            } else {
                resolved = "faker_" + fakerType + "_" + generateUuid().substr(0, 8);
            }
        } else {
            // Parse "X.y" or "X[N].y"
            auto dotPos = ref.find('.');
            if (dotPos != std::string::npos) {
                auto scope = ref.substr(0, dotPos);
                auto field = ref.substr(dotPos + 1);

                // 4. Environment variables (env.X)
                if (scope == "env") {
                    auto envIt = resolveCtx.envVars.find(field);
                    if (envIt != resolveCtx.envVars.end()) {
                        resolved = envIt->second;
                    }
                }
                // 5. Secrets (secret.X)
                else if (scope == "secret") {
                    auto secIt = resolveCtx.secrets.find(field);
                    if (secIt != resolveCtx.secrets.end()) {
                        resolved = secIt->second;
                    }
                }
                // 2. Actor sessions
                else if (!resolved) {
                    // Check if scope is an actor
                    auto actorSession = ctx.session(ActorId{scope});
                    if (actorSession) {
                        auto varIt = actorSession->variables.find(field);
                        if (varIt != actorSession->variables.end()) {
                            resolved = varIt->second;
                        }
                    }
                }
                // 3. Resource extractions (most-recent)
                if (!resolved) {
                    // Check for indexed reference: "resource[N].field"
                    static const std::regex indexedPattern(R"((\w+)\[(\d+)\])");
                    std::smatch indexMatch;
                    if (std::regex_match(scope, indexMatch, indexedPattern)) {
                        auto resName = indexMatch[1].str();
                        // Use from_chars to avoid std::stoul's exceptions.
                        auto indexStr = indexMatch[2].str();
                        std::size_t index = 0;
                        const auto* first = indexStr.data();
                        const auto* last = first + indexStr.size();
                        auto fc = std::from_chars(first, last, index);
                        if (fc.ec != std::errc{} || index == 0) {
                            // Out of range or zero — leave unresolved.
                        } else {
                            index -= 1;  // 1-indexed → 0-indexed
                            const auto& instances = ctx.instances(ResourceId{resName});
                            if (index < instances.size()) {
                                auto varIt = instances[index].variables.find(field);
                                if (varIt != instances[index].variables.end()) {
                                    resolved = varIt->second;
                                }
                            }
                        }
                    } else {
                        // Most-recent instance
                        const auto& instances = ctx.instances(ResourceId{scope});
                        if (!instances.empty()) {
                            const auto& latest = instances.back();
                            auto varIt = latest.variables.find(field);
                            if (varIt != latest.variables.end()) {
                                resolved = varIt->second;
                            }
                        }
                    }
                }
            }
        }

        if (resolved) {
            output += *resolved;
        } else {
            unresolved.push_back(ref);
            output += "{{" + ref + "}}";  // Leave unresolved in place.
        }

        lastPos = matchPos + static_cast<std::size_t>(it->length());
    }

    output += input.substr(lastPos);
    return Result{std::move(output), std::move(unresolved)};
}

}  // namespace chainapi::engine
