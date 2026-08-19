// VariableResolver — substitutes {{X.y}} references. Resolution order: builtins, sessions,
// extractions, env, secrets.
#include "VariableResolver.h"

#include "Codecs.h"

#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <format>
#include <iomanip>
#include <limits>
#include <optional>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace reqloom::engine {

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
            (digits >= 8) ? std::uint64_t{0xFFFF'FFFFU} : ((std::uint64_t{1} << (digits * 4)) - 1);
        return std::format("{:0{}x}", static_cast<std::uint64_t>(v) & mask, digits);
    };

    return hex(r(), 8) + "-" + hex(r(), 4) + "-4" + hex(r(), 3) + "-" + hex(0x8 | (r() & 0x3), 1) +
           hex(r(), 3) + "-" + hex(r(), 8) + hex(r(), 4);
}

/// ISO 8601 timestamp for a given system_clock time point (UTC, second precision).
[[nodiscard]] std::optional<std::string> formatIso(std::chrono::system_clock::time_point tp) {
    const auto time = std::chrono::system_clock::to_time_t(tp);
    std::tm buf{};
#ifdef _WIN32
    if (gmtime_s(&buf, &time) != 0) {
        return std::nullopt;
    }
#else
    if (gmtime_r(&time, &buf) == nullptr) {
        return std::nullopt;
    }
#endif
    std::ostringstream ss;
    ss << std::put_time(&buf, "%FT%TZ");
    if (!ss) {
        return std::nullopt;
    }
    return ss.str();
}

[[nodiscard]] std::optional<std::string> nowIso() {
    return formatIso(std::chrono::system_clock::now());
}

/// Parse a duration literal like "5m", "1h", "30s", "7d". Returns nullopt
/// for malformed input. Overflow-safe: absurdly large values return nullopt
/// rather than triggering signed overflow UB.
[[nodiscard]] std::optional<std::chrono::seconds> parseDuration(std::string_view literal) {
    if (literal.size() < 2) {
        return std::nullopt;
    }

    const char unit{literal.back()};
    const auto digits{literal.substr(0, literal.size() - 1)};

    long long value{};
    const auto* first{digits.data()};
    const auto* last{first + digits.size()};
    const auto fc{std::from_chars(first, last, value)};
    if (fc.ec != std::errc{} || fc.ptr != last || value < 0) {
        return std::nullopt;
    }

    const auto safeMul = [](long long v, long long factor) -> std::optional<long long> {
        if (factor == 0) {
            return 0;
        }
        if (v > std::numeric_limits<long long>::max() / factor) {
            return std::nullopt;
        }
        return v * factor;
    };

    switch (unit) {
        case 's':
            return std::chrono::seconds{value};
        case 'm':
            if (auto r = safeMul(value, 60); r) {
                return std::chrono::seconds{*r};
            }
            return std::nullopt;
        case 'h':
            if (auto r = safeMul(value, 3600); r) {
                return std::chrono::seconds{*r};
            }
            return std::nullopt;
        case 'd':
            if (auto r = safeMul(value, 86400); r) {
                return std::chrono::seconds{*r};
            }
            return std::nullopt;
        default:
            return std::nullopt;
    }
}

std::string_view trim(std::string_view s) {
    auto begin = s.find_first_not_of(" \t");
    if (begin == std::string_view::npos) {
        return {};
    }
    auto end = s.find_last_not_of(" \t");
    return s.substr(begin, end - begin + 1);
}

using ResolvedRef = std::optional<std::string>;

ResolvedRef resolveDotted(std::string_view ref,
                          const RunContext& ctx,
                          const ResolveContext& rctx,
                          int depth);

/// Single-pass `{{...}}` substitution shared by the public resolve() and
/// by the env branch (which re-expands embedded refs like a `!secret`
/// env value that parsed to `{{secret.NAME}}`). `depth` bounds the
/// re-expansion so a self-referential env value can't recurse forever.
std::string substituteRefs(std::string_view input,
                           const RunContext& ctx,
                           const ResolveContext& rctx,
                           std::vector<std::string>& unresolved,
                           int depth,
                           bool encodeResolvedRefs);

[[nodiscard]] std::optional<std::size_t> findReferenceEnd(std::string_view input,
                                                          std::size_t referenceStart) noexcept {
    char quote{};
    bool escaped{};
    for (std::size_t index = referenceStart + 2; index < input.size(); ++index) {
        const char character = input[index];
        if (quote != '\0') {
            if (escaped) {
                escaped = false;
            } else if (character == '\\') {
                escaped = true;
            } else if (character == quote) {
                quote = '\0';
            }
            continue;
        }
        if (character == '"' || character == '\'') {
            quote = character;
            continue;
        }
        if (character == '}' && index + 1 < input.size() && input[index + 1] == '}') {
            return index;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::size_t findUrlDelimiter(std::string_view input, char delimiter) noexcept {
    for (std::size_t index = 0; index < input.size(); ++index) {
        if (index + 1 < input.size() && input.substr(index, 2) == "{{") {
            const auto referenceEnd = findReferenceEnd(input, index);
            if (!referenceEnd) {
                return std::string_view::npos;
            }
            index = *referenceEnd + 1;
            continue;
        }
        if (input[index] == delimiter) {
            return index;
        }
    }
    return std::string_view::npos;
}

[[nodiscard]] bool isHexDigit(char character) noexcept {
    return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') ||
           (character >= 'A' && character <= 'F');
}

/// Encode resolved URL components without encoding an existing `%HH` escape again.
[[nodiscard]] std::string urlEncodePreservingEscapes(std::string_view input) {
    std::string output;
    output.reserve(input.size());
    std::size_t chunkStart{};
    for (std::size_t index = 0; index < input.size(); ++index) {
        if (input[index] != '%' || index + 2 >= input.size() || !isHexDigit(input[index + 1]) ||
            !isHexDigit(input[index + 2])) {
            continue;
        }
        output += urlEncode(input.substr(chunkStart, index - chunkStart));
        output.append(input.substr(index, 3));
        index += 2;
        chunkStart = index + 1;
    }
    output += urlEncode(input.substr(chunkStart));
    return output;
}

[[nodiscard]] bool isWholeReference(std::string_view input) noexcept {
    if (input.size() <= 4 || !input.starts_with("{{")) {
        return false;
    }
    const auto referenceEnd{findReferenceEnd(input, 0)};
    return referenceEnd && *referenceEnd + 2 == input.size();
}

[[nodiscard]] bool isSafeWholePathOutput(std::string_view path) noexcept {
    if (path.empty() || path.front() != '/' || path.starts_with("//")) {
        return false;
    }
    for (const char character : path) {
        const auto byte = static_cast<unsigned char>(character);
        if (character == '\\' || byte <= 0x20U || byte == 0x7FU) {
            return false;
        }
    }
    return true;
}

// Re-expansion of an env value into one further level (env → secret) is
// the only nesting we rely on; a small cap leaves margin without cycles.
constexpr int kMaxResolveDepth = 4;

struct CallParts {
    std::string_view name;
    std::string_view arg;
};

std::optional<CallParts> splitCall(std::string_view tail) {
    const auto open = tail.find('(');
    if (open == std::string_view::npos) {
        return std::nullopt;
    }
    if (tail.back() != ')') {
        return std::nullopt;
    }

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
                           const ResolveContext& rctx,
                           int depth) {
    arg = trim(arg);
    if (arg.empty()) {
        return std::nullopt;
    }

    if (arg.front() == '"' || arg.front() == '\'') {
        const char quote = arg.front();
        if (arg.size() < 2 || arg.back() != quote) {
            return std::nullopt;
        }

        std::string value;
        value.reserve(arg.size() - 2);
        bool escaped{};
        for (const char character : arg.substr(1, arg.size() - 2)) {
            if (escaped) {
                if (character != quote && character != '\\') {
                    value.push_back('\\');
                }
                value.push_back(character);
                escaped = false;
            } else if (character == '\\') {
                escaped = true;
            } else if (character == quote) {
                return std::nullopt;
            } else {
                value.push_back(character);
            }
        }
        if (escaped) {
            return std::nullopt;
        }
        return value;
    }

    return resolveDotted(arg, ctx, rctx, depth);
}

/// Resolve `$.something[+offset]` builtins (category 1 in the resolution
/// order). Returns nullopt for unrecognised builtins.
ResolvedRef resolveBuiltin(std::string_view ref,
                           const RunContext& ctx,
                           const ResolveContext& rctx,
                           int depth) {
    if (!ref.starts_with("$.")) {
        return std::nullopt;
    }

    // Function-call form: `$.ns.name(arg)`. Dispatch first because the
    // offset-stripping below must not run inside parentheses.
    if (ref.find('(') != std::string_view::npos) {
        if (ref.starts_with("$.base64.")) {
            const auto call = splitCall(ref.substr(9));
            if (!call) {
                return std::nullopt;
            }
            const auto value = resolveCallArg(call->arg, ctx, rctx, depth);
            if (!value) {
                return std::nullopt;
            }
            if (call->name == "encode") {
                return base64Encode(*value);
            }
            if (call->name == "decode") {
                return base64Decode(*value);
            }
            return std::nullopt;
        }
        if (ref.starts_with("$.hex.")) {
            const auto call = splitCall(ref.substr(6));
            if (!call) {
                return std::nullopt;
            }
            const auto value = resolveCallArg(call->arg, ctx, rctx, depth);
            if (!value) {
                return std::nullopt;
            }
            if (call->name == "encode") {
                return hexEncode(*value);
            }
            if (call->name == "decode") {
                return hexDecode(*value);
            }
            return std::nullopt;
        }
        if (ref.starts_with("$.url.")) {
            const auto call = splitCall(ref.substr(6));
            if (!call) {
                return std::nullopt;
            }
            const auto value = resolveCallArg(call->arg, ctx, rctx, depth);
            if (!value) {
                return std::nullopt;
            }
            if (call->name == "encode") {
                return urlEncode(*value);
            }
            if (call->name == "decode") {
                return urlDecode(*value);
            }
            return std::nullopt;
        }
        return std::nullopt;
    }

    // Split off optional ±offset. Find the rightmost '+' or '-' not inside parens.
    std::chrono::seconds offset{0};
    bool hasOffset = false;
    {
        int parenDepth = 0;
        std::size_t opPos = std::string_view::npos;
        char opCh = '\0';
        for (std::size_t i = 0; i < ref.size(); ++i) {
            const char c = ref[i];
            if (c == '(') {
                ++parenDepth;
            } else if (c == ')') {
                --parenDepth;
            } else if (parenDepth == 0 && (c == '+' || c == '-') && i > 1) {
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
        if (hasOffset) {
            return std::nullopt;
        }
        return generateUuid();
    }
    if (ref == "$.now") {
        if (!hasOffset) {
            return nowIso();
        }

        using Clock = std::chrono::system_clock;
        using ClockDuration = Clock::duration;
        const auto maxSeconds =
            std::chrono::duration_cast<std::chrono::seconds>(ClockDuration::max()).count();
        const auto minSeconds =
            std::chrono::duration_cast<std::chrono::seconds>(ClockDuration::min()).count();
        if (offset.count() > maxSeconds || offset.count() < minSeconds) {
            return std::nullopt;
        }

        const ClockDuration delta = std::chrono::duration_cast<ClockDuration>(offset);
        const ClockDuration current = Clock::now().time_since_epoch();
        if ((delta > ClockDuration::zero() && current > ClockDuration::max() - delta) ||
            (delta < ClockDuration::zero() && current < ClockDuration::min() - delta)) {
            return std::nullopt;
        }
        return formatIso(Clock::time_point{current + delta});
    }
    if (ref.starts_with("$.env.")) {
        if (hasOffset) {
            return std::nullopt;
        }
        const std::string envName{ref.substr(6)};
        if (auto* val = std::getenv(envName.c_str())) {
            return std::string{val};
        }
        return std::nullopt;
    }
    if (ref.starts_with("$.faker.")) {
        if (hasOffset) {
            return std::nullopt;
        }
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
ResolvedRef resolveDotted(std::string_view ref,
                          const RunContext& ctx,
                          const ResolveContext& rctx,
                          int depth) {
    const auto dotPos = ref.find('.');
    if (dotPos == std::string_view::npos) {
        return std::nullopt;
    }

    const auto scope = std::string{ref.substr(0, dotPos)};
    const auto field = std::string{ref.substr(dotPos + 1)};

    if (scope == "env") {
        auto it = rctx.envVars.find(field);
        if (it == rctx.envVars.end()) {
            return std::nullopt;
        }
        // An env value may itself be a reference — e.g. a `!secret NAME`
        // entry parsed to `{{secret.NAME}}`. Re-expand one level deeper
        // (bounded by kMaxResolveDepth) so callers see the resolved
        // secret, not the literal placeholder. If the nested value can't
        // fully resolve (e.g. the secret wasn't loaded), report this env
        // ref as unresolved rather than emitting a half-expanded string —
        // the outer loop then preserves the `{{env.X}}` placeholder and
        // records it, matching how every other unresolved ref behaves.
        if (it->second.find("{{") != std::string::npos) {
            std::vector<std::string> nested;
            auto expanded = substituteRefs(it->second, ctx, rctx, nested, depth + 1, false);
            if (!nested.empty()) {
                return std::nullopt;
            }
            return expanded;
        }
        return it->second;
    }

    if (scope == "secret") {
        auto it = rctx.secrets.find(field);
        if (it != rctx.secrets.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    if (const auto* session = ctx.session(ActorId{scope}); session != nullptr) {
        auto it = session->variables.find(field);
        if (it != session->variables.end()) {
            return it->second;
        }
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
        if (fc.ec != std::errc{} || index == 0) {
            return std::nullopt;
        }

        index -= 1;  // 1-indexed → 0-indexed
        const auto& instances = ctx.instances(ResourceId{resName});
        if (index >= instances.size()) {
            return std::nullopt;
        }

        auto it = instances[index].variables.find(field);
        if (it != instances[index].variables.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    // Search instances in reverse (most recent first). Handles the case where
    // a resource accrues fields across multiple operations — e.g. order.create
    // extracts order_id, order.pay extracts payment_id; both should resolve.
    const ResourceId resId{scope};
    const auto& instances = ctx.instances(resId);

    // For-each binding: while iterating, resolve to the bound instance instead
    // of the most-recent, so the body uses the current item.
    if (const auto iter = ctx.iteration(resId); iter.has_value() && *iter < instances.size()) {
        const auto fieldIt = instances[*iter].variables.find(field);
        if (fieldIt != instances[*iter].variables.end()) {
            return fieldIt->second;
        }
        return std::nullopt;
    }

    for (auto it = instances.rbegin(); it != instances.rend(); ++it) {
        auto fieldIt = it->variables.find(field);
        if (fieldIt != it->variables.end()) {
            return fieldIt->second;
        }
    }

    return std::nullopt;
}

std::string substituteRefs(std::string_view input,
                           const RunContext& ctx,
                           const ResolveContext& rctx,
                           std::vector<std::string>& unresolved,
                           int depth,
                           bool encodeResolvedRefs) {
    // Depth guard: a self-referential env value (e.g. one whose value
    // references itself) would otherwise loop. Preserve the text but report
    // the nested reference so callers fail closed at the cap.
    if (depth >= kMaxResolveDepth) {
        const std::size_t referenceStart{input.find("{{")};
        if (referenceStart != std::string_view::npos) {
            unresolved.emplace_back(input.substr(referenceStart));
        }
        return std::string{input};
    }

    std::string output;
    output.reserve(input.size());
    std::size_t offset{};
    while (offset < input.size()) {
        const std::size_t referenceStart = input.find("{{", offset);
        if (referenceStart == std::string_view::npos) {
            output.append(input.substr(offset));
            break;
        }
        output.append(input.substr(offset, referenceStart - offset));

        const auto referenceEnd = findReferenceEnd(input, referenceStart);
        if (!referenceEnd) {
            unresolved.emplace_back(input.substr(referenceStart));
            output.append(input.substr(referenceStart));
            break;
        }

        const std::string_view rawRef =
            input.substr(referenceStart + 2, *referenceEnd - referenceStart - 2);
        const std::string trimmedRef{trim(rawRef)};
        ResolvedRef resolved;
        if (!trimmedRef.empty()) {
            resolved = resolveBuiltin(trimmedRef, ctx, rctx, depth);
            if (!resolved) {
                resolved = resolveDotted(trimmedRef, ctx, rctx, depth);
            }
        }

        if (resolved) {
            output += encodeResolvedRefs ? urlEncodePreservingEscapes(*resolved) : *resolved;
        } else {
            unresolved.push_back(trimmedRef);
            output.append(input.substr(referenceStart, *referenceEnd + 2 - referenceStart));
        }
        offset = *referenceEnd + 2;
    }
    return output;
}

}  // namespace

VariableResolver::VariableResolver() = default;

VariableResolver::Result VariableResolver::resolve(std::string_view templateStr,
                                                   const RunContext& ctx,
                                                   const ResolveContext& resolveCtx) const {
    std::vector<std::string> unresolved;
    std::string output = substituteRefs(templateStr, ctx, resolveCtx, unresolved, 0, false);
    return Result{std::move(output), std::move(unresolved)};
}

VariableResolver::Result VariableResolver::resolveUrlPath(std::string_view templateStr,
                                                          const RunContext& ctx,
                                                          const ResolveContext& resolveCtx) const {
    const auto candidateQueryStart = findUrlDelimiter(templateStr, '?');
    const auto fragmentStart = findUrlDelimiter(templateStr, '#');
    const bool hasQuery =
        candidateQueryStart != std::string_view::npos &&
        (fragmentStart == std::string_view::npos || candidateQueryStart < fragmentStart);
    const auto queryStart = hasQuery ? candidateQueryStart : std::string_view::npos;
    const auto pathEnd =
        hasQuery ? queryStart
                 : (fragmentStart == std::string_view::npos ? templateStr.size() : fragmentStart);

    std::vector<std::string> unresolved;
    std::string output;
    output.reserve(templateStr.size());
    const auto pathTemplate = templateStr.substr(0, pathEnd);
    const bool wholePathReference = isWholeReference(pathTemplate);
    const std::string resolvedPath =
        substituteRefs(pathTemplate, ctx, resolveCtx, unresolved, 0, !wholePathReference);
    if (wholePathReference && unresolved.empty() && !isSafeWholePathOutput(resolvedPath)) {
        unresolved.emplace_back(trim(pathTemplate.substr(2, pathTemplate.size() - 4)));
        output.append(pathTemplate);
    } else {
        output += resolvedPath;
    }

    if (hasQuery) {
        const auto queryEnd =
            fragmentStart == std::string_view::npos ? templateStr.size() : fragmentStart;
        output.push_back('?');
        output += substituteRefs(templateStr.substr(queryStart + 1, queryEnd - queryStart - 1),
                                 ctx,
                                 resolveCtx,
                                 unresolved,
                                 0,
                                 true);
    }
    if (fragmentStart != std::string_view::npos) {
        output += substituteRefs(
            templateStr.substr(fragmentStart), ctx, resolveCtx, unresolved, 0, false);
    }
    return Result{std::move(output), std::move(unresolved)};
}

}  // namespace reqloom::engine
