// VariableResolver — substitutes {{X.y}} references.
//
// Resolution order (first match wins):
//   1. Builtins ($.now, $.now±Nu, $.uuid, $.faker.*, $.env.*,
//                $.base64.*, $.hex.*, $.url.*)
//   2. Actor sessions
//   3. Resource extractions (most-recent or indexed)
//   4. Environment variables (env.X)
//   5. Secrets (secret.X)
//
// Reference grammar:
//   ref       := builtin | dotted | indexed
//   builtin   := '$.' name ('.' name)? ('(' arg ')')? (offset)?
//   offset    := '+' duration | '-' duration
//   duration  := digits ('s' | 'm' | 'h' | 'd')
//   dotted    := name '.' name        // env.X, secret.X, actor.var, resource.var
//   indexed   := name '[' digits ']' '.' name   // resource[N].var
#include "VariableResolver.h"

#include "Codecs.h"

#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <limits>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>

namespace chainapi::engine {

namespace {

using namespace codecs;

/// Generate a UUID v4 string. RNG is thread_local to avoid races.
std::string generateUuid() {
    thread_local std::mt19937_64 gen{std::random_device{}()};
    std::uniform_int_distribution<std::uint32_t> dist;

    auto r = [&]() {
        return dist(gen);
    };
    auto hex = [](std::uint32_t v, int digits) {
        const std::uint64_t mask =
            (digits >= 8) ? std::uint64_t{0xFFFF'FFFFu} : ((std::uint64_t{1} << (digits * 4)) - 1);
        std::ostringstream ss;
        ss << std::hex << std::setfill('0') << std::setw(digits)
           << (static_cast<std::uint64_t>(v) & mask);
        return ss.str();
    };

    return hex(r(), 8) + "-" + hex(r(), 4) + "-4" + hex(r(), 3) + "-" + hex(0x8 | (r() & 0x3), 1) +
           hex(r(), 3) + "-" + hex(r(), 8) + hex(r(), 4);
}

/// ISO 8601 timestamp for a given system_clock time point (UTC, second precision).
std::string formatIso(std::chrono::system_clock::time_point tp) {
    auto time = std::chrono::system_clock::to_time_t(tp);
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

std::string nowIso() {
    return formatIso(std::chrono::system_clock::now());
}

/// Parse a duration literal like "5m", "1h", "30s", "7d". Returns nullopt
/// for malformed input. Overflow-safe: absurdly large values return nullopt
/// rather than triggering signed overflow UB.
std::optional<std::chrono::seconds> parseDuration(std::string_view literal) {
    if (literal.size() < 2) return std::nullopt;

    const char unit = literal.back();
    auto digits = literal.substr(0, literal.size() - 1);

    long long value = 0;
    const auto* first = digits.data();
    const auto* last = first + digits.size();
    auto fc = std::from_chars(first, last, value);
    if (fc.ec != std::errc{} || fc.ptr != last || value < 0) {
        return std::nullopt;
    }

    auto safeMul = [](long long v, long long factor) -> std::optional<long long> {
        if (factor == 0) return 0;
        if (v > std::numeric_limits<long long>::max() / factor) {
            return std::nullopt;
        }
        return v * factor;
    };

    switch (unit) {
        case 's':
            return std::chrono::seconds{value};
        case 'm':
            if (auto r = safeMul(value, 60); r) return std::chrono::seconds{*r};
            return std::nullopt;
        case 'h':
            if (auto r = safeMul(value, 3600); r) return std::chrono::seconds{*r};
            return std::nullopt;
        case 'd':
            if (auto r = safeMul(value, 86400); r) return std::chrono::seconds{*r};
            return std::nullopt;
        default:
            return std::nullopt;
    }
}

std::string_view trim(std::string_view s) {
    auto begin = s.find_first_not_of(" \t");
    if (begin == std::string_view::npos) return {};
    auto end = s.find_last_not_of(" \t");
    return s.substr(begin, end - begin + 1);
}

using ResolvedRef = std::optional<std::string>;

ResolvedRef resolveDotted(std::string_view ref, const RunContext& ctx, const ResolveContext& rctx);

struct CallParts {
    std::string_view name;
    std::string_view arg;
};

std::optional<CallParts> splitCall(std::string_view tail) {
    const auto open = tail.find('(');
    if (open == std::string_view::npos) return std::nullopt;
    if (tail.back() != ')') return std::nullopt;

    CallParts parts;
    parts.name = trim(tail.substr(0, open));
    parts.arg = trim(tail.substr(open + 1, tail.size() - open - 2));
    return parts;
}

/// Evaluate a single function argument. Supported forms:
///   - "literal" / 'literal' → returns the literal text
///   - bareReference         → resolves env.X / secret.X / actor.var / resource.var
ResolvedRef resolveCallArg(std::string_view arg,
                           const RunContext& ctx,
                           const ResolveContext& rctx) {
    arg = trim(arg);
    if (arg.empty()) return std::nullopt;

    if ((arg.front() == '"' && arg.back() == '"') || (arg.front() == '\'' && arg.back() == '\'')) {
        if (arg.size() < 2) return std::nullopt;
        return std::string{arg.substr(1, arg.size() - 2)};
    }

    return resolveDotted(arg, ctx, rctx);
}

/// Resolve `$.something[+offset]` builtins (category 1 in the resolution
/// order). Returns nullopt for unrecognised builtins.
ResolvedRef resolveBuiltin(std::string_view ref,
                           const RunContext& ctx,
                           const ResolveContext& rctx) {
    if (!ref.starts_with("$.")) return std::nullopt;

    // Function-call form: `$.ns.name(arg)`. Dispatch first because the
    // offset-stripping below must not run inside parentheses.
    if (ref.find('(') != std::string_view::npos) {
        if (ref.starts_with("$.base64.")) {
            const auto call = splitCall(ref.substr(9));
            if (!call) return std::nullopt;
            const auto value = resolveCallArg(call->arg, ctx, rctx);
            if (!value) return std::nullopt;
            if (call->name == "encode") return base64Encode(*value);
            if (call->name == "decode") return base64Decode(*value);
            return std::nullopt;
        }
        if (ref.starts_with("$.hex.")) {
            const auto call = splitCall(ref.substr(6));
            if (!call) return std::nullopt;
            const auto value = resolveCallArg(call->arg, ctx, rctx);
            if (!value) return std::nullopt;
            if (call->name == "encode") return hexEncode(*value);
            if (call->name == "decode") return hexDecode(*value);
            return std::nullopt;
        }
        if (ref.starts_with("$.url.")) {
            const auto call = splitCall(ref.substr(6));
            if (!call) return std::nullopt;
            const auto value = resolveCallArg(call->arg, ctx, rctx);
            if (!value) return std::nullopt;
            if (call->name == "encode") return urlEncode(*value);
            if (call->name == "decode") return urlDecode(*value);
            return std::nullopt;
        }
        return std::nullopt;
    }

    // Split off optional ±offset. Find the rightmost '+' or '-' not inside parens.
    std::chrono::seconds offset{0};
    bool hasOffset = false;
    {
        int depth = 0;
        std::size_t opPos = std::string_view::npos;
        char opCh = '\0';
        for (std::size_t i = 0; i < ref.size(); ++i) {
            const char c = ref[i];
            if (c == '(')
                ++depth;
            else if (c == ')')
                --depth;
            else if (depth == 0 && (c == '+' || c == '-') && i > 1) {
                opPos = i;
                opCh = c;
            }
        }
        if (opPos != std::string_view::npos) {
            auto durationLit = trim(ref.substr(opPos + 1));
            auto duration = parseDuration(durationLit);
            if (duration) {
                offset = (opCh == '+') ? *duration : -*duration;
                ref = trim(ref.substr(0, opPos));
                hasOffset = true;
            }
        }
    }

    if (ref == "$.uuid") {
        if (hasOffset) return std::nullopt;
        return generateUuid();
    }
    if (ref == "$.now") {
        if (!hasOffset) return nowIso();
        return formatIso(std::chrono::system_clock::now() + offset);
    }
    if (ref.starts_with("$.env.")) {
        if (hasOffset) return std::nullopt;
        const std::string envName{ref.substr(6)};
        if (auto* val = std::getenv(envName.c_str())) {
            return std::string{val};
        }
        return std::nullopt;
    }
    if (ref.starts_with("$.faker.")) {
        if (hasOffset) return std::nullopt;
        const auto fakerType = ref.substr(8);
        if (fakerType == "email") {
            return "test+" + generateUuid().substr(0, 8) + "@example.com";
        }
        if (fakerType == "phone") {
            // Weak RNG is fine here — faker is for fixture data, not security.
            return "+1555" + std::to_string(std::random_device{}() % 10000000);
        }
        return std::string{"faker_"} + std::string{fakerType} + "_" + generateUuid().substr(0, 8);
    }

    return std::nullopt;
}

/// Resolve dotted refs (env.X, secret.X, actor.var, resource.var) and
/// indexed refs (resource[N].var).
ResolvedRef resolveDotted(std::string_view ref, const RunContext& ctx, const ResolveContext& rctx) {
    const auto dotPos = ref.find('.');
    if (dotPos == std::string_view::npos) return std::nullopt;

    const auto scope = std::string{ref.substr(0, dotPos)};
    const auto field = std::string{ref.substr(dotPos + 1)};

    if (scope == "env") {
        auto it = rctx.envVars.find(field);
        if (it != rctx.envVars.end()) return it->second;
        return std::nullopt;
    }

    if (scope == "secret") {
        auto it = rctx.secrets.find(field);
        if (it != rctx.secrets.end()) return it->second;
        return std::nullopt;
    }

    if (auto* session = ctx.session(ActorId{scope}); session != nullptr) {
        auto it = session->variables.find(field);
        if (it != session->variables.end()) return it->second;
    }

    // Indexed resource reference: resource[N].var (1-indexed).
    static const std::regex indexedPattern(R"((\w+)\[(\d+)\])");
    std::smatch indexMatch;
    if (std::regex_match(scope, indexMatch, indexedPattern)) {
        const auto resName = indexMatch[1].str();
        const auto indexStr = indexMatch[2].str();
        std::size_t index = 0;
        const auto* first = indexStr.data();
        const auto* last = first + indexStr.size();
        auto fc = std::from_chars(first, last, index);
        if (fc.ec != std::errc{} || index == 0) return std::nullopt;

        index -= 1;  // 1-indexed → 0-indexed
        const auto& instances = ctx.instances(ResourceId{resName});
        if (index >= instances.size()) return std::nullopt;

        auto it = instances[index].variables.find(field);
        if (it != instances[index].variables.end()) return it->second;
        return std::nullopt;
    }

    // Search instances in reverse (most recent first). Handles the case where
    // a resource accrues fields across multiple operations — e.g. order.create
    // extracts order_id, order.pay extracts payment_id; both should resolve.
    const auto& instances = ctx.instances(ResourceId{scope});
    for (auto it = instances.rbegin(); it != instances.rend(); ++it) {
        auto fieldIt = it->variables.find(field);
        if (fieldIt != it->variables.end()) return fieldIt->second;
    }

    return std::nullopt;
}

}  // namespace

VariableResolver::VariableResolver() = default;
VariableResolver::~VariableResolver() = default;

VariableResolver::Result VariableResolver::resolve(std::string_view templateStr,
                                                   const RunContext& ctx,
                                                   const ResolveContext& resolveCtx) const {
    static const std::regex refPattern(R"(\{\{([^}]+)\}\})");
    const std::string input(templateStr);
    std::string output;
    std::vector<std::string> unresolved;

    std::sregex_iterator begin(input.begin(), input.end(), refPattern);
    std::sregex_iterator end;
    std::size_t lastPos = 0;

    for (auto it = begin; it != end; ++it) {
        const auto matchPos = static_cast<std::size_t>(it->position());
        output += input.substr(lastPos, matchPos - lastPos);

        const auto rawRef = (*it)[1].str();
        const auto trimmed = std::string{trim(rawRef)};

        ResolvedRef resolved = resolveBuiltin(trimmed, ctx, resolveCtx);
        if (!resolved) {
            resolved = resolveDotted(trimmed, ctx, resolveCtx);
        }

        if (resolved) {
            output += *resolved;
        } else {
            unresolved.push_back(trimmed);
            output += "{{" + trimmed + "}}";
        }

        lastPos = matchPos + static_cast<std::size_t>(it->length());
    }

    output += input.substr(lastPos);
    return Result{std::move(output), std::move(unresolved)};
}

}  // namespace chainapi::engine
