// VariableResolver — substitutes {{X.y}} references per PRD §5.7 and
// Engine Req §3.10.
//
// Resolution order (first match wins — AC-3.10.1):
//   1. Builtins ($.now, $.now±Nu, $.uuid, $.faker.*, $.env.*,
//                $.base64.*, $.hex.*, $.url.*)
//   2. Actor sessions
//   3. Resource extractions (most-recent or indexed)
//   4. Environment variables (env.X)
//   5. Secrets (secret.X)
//
// Reference grammar (PRD §5.7):
//   ref       := builtin | dotted | indexed
//   builtin   := '$.' name ('.' name)? ('(' arg ')')? (offset)?
//   offset    := '+' duration | '-' duration
//   duration  := digits ('s' | 'm' | 'h' | 'd')
//   dotted    := name '.' name        // env.X, secret.X, actor.var, resource.var
//   indexed   := name '[' digits ']' '.' name   // resource[N].var
//
// Single-arg function calls (`$.base64.encode(env.X)`) are wired here.
// Crypto-dependent builtins ($.hmac, $.jwt, $.hash) and content-aware
// helpers ($.json.stringify) are intentionally NOT exposed at template
// scope — they live on the hook `ctx` object (Slice 5b) where the
// request body is reachable as structured data and where the OpenSSL
// dep belongs.
#include "VariableResolver.h"

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

/// ISO 8601 timestamp for a given system_clock time point (UTC, second
/// precision). Matches the format used by `nowIso()` so consumers can
/// rely on a single shape across `$.now` and `$.now±Nu`.
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

/// Parse a duration literal like "5m", "1h", "30s", "7d". Returns
/// nullopt for malformed input. Negative numbers are not accepted —
/// the offset sign is carried separately by the caller.
///
/// Overflow-safe: an absurdly large numeric component (e.g. "999999999d")
/// returns nullopt rather than triggering signed overflow UB.
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

    // Saturating multiply against a fixed factor. Returns nullopt on
    // overflow rather than letting `value * factor` invoke UB.
    auto safeMul = [](long long v, long long factor)
        -> std::optional<long long> {
        if (factor == 0) return 0;
        if (v > std::numeric_limits<long long>::max() / factor) {
            return std::nullopt;
        }
        return v * factor;
    };

    switch (unit) {
        case 's': return std::chrono::seconds{value};
        case 'm':
            if (auto r = safeMul(value, 60); r) return std::chrono::seconds{*r};
            return std::nullopt;
        case 'h':
            if (auto r = safeMul(value, 3600); r) return std::chrono::seconds{*r};
            return std::nullopt;
        case 'd':
            if (auto r = safeMul(value, 86400); r) return std::chrono::seconds{*r};
            return std::nullopt;
        default: return std::nullopt;
    }
}

/// Trim leading and trailing ASCII whitespace.
std::string_view trim(std::string_view s) {
    auto begin = s.find_first_not_of(" \t");
    if (begin == std::string_view::npos) return {};
    auto end = s.find_last_not_of(" \t");
    return s.substr(begin, end - begin + 1);
}

// ─── String codecs ──────────────────────────────────────────────────────────
// Standard alphabet base64 with padding. PRD §5.7.

constexpr std::string_view kBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(std::string_view input) {
    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);

    std::size_t i = 0;
    while (i + 3 <= input.size()) {
        const auto b0 = static_cast<std::uint8_t>(input[i]);
        const auto b1 = static_cast<std::uint8_t>(input[i + 1]);
        const auto b2 = static_cast<std::uint8_t>(input[i + 2]);
        out.push_back(kBase64Alphabet[(b0 >> 2) & 0x3F]);
        out.push_back(kBase64Alphabet[((b0 << 4) | (b1 >> 4)) & 0x3F]);
        out.push_back(kBase64Alphabet[((b1 << 2) | (b2 >> 6)) & 0x3F]);
        out.push_back(kBase64Alphabet[b2 & 0x3F]);
        i += 3;
    }

    if (i < input.size()) {
        const auto b0 = static_cast<std::uint8_t>(input[i]);
        out.push_back(kBase64Alphabet[(b0 >> 2) & 0x3F]);
        if (i + 1 == input.size()) {
            out.push_back(kBase64Alphabet[(b0 << 4) & 0x3F]);
            out.push_back('=');
            out.push_back('=');
        } else {
            const auto b1 = static_cast<std::uint8_t>(input[i + 1]);
            out.push_back(kBase64Alphabet[((b0 << 4) | (b1 >> 4)) & 0x3F]);
            out.push_back(kBase64Alphabet[(b1 << 2) & 0x3F]);
            out.push_back('=');
        }
    }
    return out;
}

/// Lookup table from ASCII byte to 6-bit base64 value (or 0xFF for invalid).
constexpr std::array<std::uint8_t, 256> makeBase64DecodeTable() {
    std::array<std::uint8_t, 256> t{};
    for (auto& v : t) v = 0xFF;
    for (std::uint8_t i = 0; i < kBase64Alphabet.size(); ++i) {
        t[static_cast<std::uint8_t>(kBase64Alphabet[i])] = i;
    }
    return t;
}

std::optional<std::string> base64Decode(std::string_view input) {
    static constexpr auto kTable = makeBase64DecodeTable();

    // Strip padding for length math; reject any non-alphabet, non-padding char.
    std::string buf;
    buf.reserve(input.size());
    int padding = 0;
    for (const char c : input) {
        if (c == '=') {
            ++padding;
            continue;
        }
        if (padding > 0) return std::nullopt;  // chars after padding
        if (kTable[static_cast<std::uint8_t>(c)] == 0xFF) {
            // Allow whitespace inside encoded blocks for robustness.
            if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
            return std::nullopt;
        }
        buf.push_back(c);
    }
    if (padding > 2) return std::nullopt;

    std::string out;
    out.reserve((buf.size() * 3) / 4);

    std::size_t i = 0;
    while (i + 4 <= buf.size()) {
        const auto v0 = kTable[static_cast<std::uint8_t>(buf[i])];
        const auto v1 = kTable[static_cast<std::uint8_t>(buf[i + 1])];
        const auto v2 = kTable[static_cast<std::uint8_t>(buf[i + 2])];
        const auto v3 = kTable[static_cast<std::uint8_t>(buf[i + 3])];
        out.push_back(static_cast<char>((v0 << 2) | (v1 >> 4)));
        out.push_back(static_cast<char>((v1 << 4) | (v2 >> 2)));
        out.push_back(static_cast<char>((v2 << 6) | v3));
        i += 4;
    }

    const auto rem = buf.size() - i;
    if (rem == 0) return out;
    if (rem == 1) return std::nullopt;  // invalid: a single base64 char is < 6 bits
    if (rem == 2) {
        const auto v0 = kTable[static_cast<std::uint8_t>(buf[i])];
        const auto v1 = kTable[static_cast<std::uint8_t>(buf[i + 1])];
        out.push_back(static_cast<char>((v0 << 2) | (v1 >> 4)));
    } else {  // rem == 3
        const auto v0 = kTable[static_cast<std::uint8_t>(buf[i])];
        const auto v1 = kTable[static_cast<std::uint8_t>(buf[i + 1])];
        const auto v2 = kTable[static_cast<std::uint8_t>(buf[i + 2])];
        out.push_back(static_cast<char>((v0 << 2) | (v1 >> 4)));
        out.push_back(static_cast<char>((v1 << 4) | (v2 >> 2)));
    }
    return out;
}

std::string hexEncode(std::string_view input) {
    constexpr std::string_view kHex = "0123456789abcdef";
    std::string out;
    out.reserve(input.size() * 2);
    for (const char c : input) {
        const auto b = static_cast<std::uint8_t>(c);
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

std::optional<std::string> hexDecode(std::string_view input) {
    if (input.size() % 2 != 0) return std::nullopt;

    auto value = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    std::string out;
    out.reserve(input.size() / 2);
    for (std::size_t i = 0; i < input.size(); i += 2) {
        const int hi = value(input[i]);
        const int lo = value(input[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return out;
}

/// Percent-encode for URL path / query component scope per RFC 3986.
/// Unreserved characters (A-Z a-z 0-9 - _ . ~) pass through; everything
/// else is %HH.
std::string urlEncode(std::string_view input) {
    constexpr std::string_view kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(input.size());
    for (const char c : input) {
        const auto b = static_cast<std::uint8_t>(c);
        const bool unreserved =
            (b >= 'A' && b <= 'Z') ||
            (b >= 'a' && b <= 'z') ||
            (b >= '0' && b <= '9') ||
            b == '-' || b == '_' || b == '.' || b == '~';
        if (unreserved) {
            out.push_back(c);
        } else {
            out.push_back('%');
            out.push_back(kHex[b >> 4]);
            out.push_back(kHex[b & 0x0F]);
        }
    }
    return out;
}

std::optional<std::string> urlDecode(std::string_view input) {
    auto value = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    std::string out;
    out.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        const char c = input[i];
        if (c == '+') {
            // Common application/x-www-form-urlencoded convention. Accept
            // both `+` and `%20` as space on decode.
            out.push_back(' ');
        } else if (c == '%') {
            if (i + 2 >= input.size()) return std::nullopt;
            const int hi = value(input[i + 1]);
            const int lo = value(input[i + 2]);
            if (hi < 0 || lo < 0) return std::nullopt;
            out.push_back(static_cast<char>((hi << 4) | lo));
            i += 2;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

/// Result of trying to resolve a single reference. The resolver returns
/// nullopt when the reference is structurally valid but no value is
/// available; the caller then records it in `unresolved`.
using ResolvedRef = std::optional<std::string>;

// Forward declaration: resolveDotted is used by resolveCallArg to handle
// references inside function arguments.
ResolvedRef resolveDotted(std::string_view ref,
                          const RunContext& ctx,
                          const ResolveContext& rctx);

/// Split a `$.namespace.name(arg)` reference into `(name, arg)` parts.
/// Returns nullopt when the reference is not a function call. The
/// outer `$.namespace.` prefix is matched by the caller.
struct CallParts {
    std::string_view name;  ///< e.g. "encode" for "$.base64.encode(x)"
    std::string_view arg;   ///< raw arg text, leading/trailing whitespace trimmed
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
///   - "literal"           → returns the literal text
///   - 'literal'           → returns the literal text
///   - bareReference       → resolves env.X / secret.X / actor.var / resource.var
///
/// Returns nullopt for malformed input or unresolved references. Builtins
/// inside arguments (`$.base64.encode($.now)`) are NOT supported in MVP —
/// keep the grammar intentionally shallow.
ResolvedRef resolveCallArg(std::string_view arg,
                           const RunContext& ctx,
                           const ResolveContext& rctx) {
    arg = trim(arg);
    if (arg.empty()) return std::nullopt;

    // Quoted literal — strip the surrounding quotes.
    if ((arg.front() == '"' && arg.back() == '"') ||
        (arg.front() == '\'' && arg.back() == '\'')) {
        if (arg.size() < 2) return std::nullopt;
        return std::string{arg.substr(1, arg.size() - 2)};
    }

    // Bare reference — delegate to the dotted resolver. Builtins ($.now,
    // $.uuid, etc.) inside args are deliberately not supported; that's
    // composition we don't need yet.
    return resolveDotted(arg, ctx, rctx);
}

/// Resolve `$.something[+offset]` builtins (category 1 in the resolution
/// order). Returns nullopt for unrecognised builtins so the caller can
/// fall through to the dotted/indexed paths.
ResolvedRef resolveBuiltin(std::string_view ref,
                           const RunContext& ctx,
                           const ResolveContext& rctx) {
    if (!ref.starts_with("$.")) return std::nullopt;

    // Function-call form: `$.ns.name(arg)`. Dispatch first because the
    // offset-stripping below is meaningless for codec calls and must
    // not run inside parentheses.
    if (ref.find('(') != std::string_view::npos) {
        if (ref.starts_with("$.base64.")) {
            const auto call = splitCall(ref.substr(9));  // strip "$.base64."
            if (!call) return std::nullopt;
            const auto value = resolveCallArg(call->arg, ctx, rctx);
            if (!value) return std::nullopt;
            if (call->name == "encode") return base64Encode(*value);
            if (call->name == "decode") return base64Decode(*value);
            return std::nullopt;
        }
        if (ref.starts_with("$.hex.")) {
            const auto call = splitCall(ref.substr(6));   // strip "$.hex."
            if (!call) return std::nullopt;
            const auto value = resolveCallArg(call->arg, ctx, rctx);
            if (!value) return std::nullopt;
            if (call->name == "encode") return hexEncode(*value);
            if (call->name == "decode") return hexDecode(*value);
            return std::nullopt;
        }
        if (ref.starts_with("$.url.")) {
            const auto call = splitCall(ref.substr(6));   // strip "$.url."
            if (!call) return std::nullopt;
            const auto value = resolveCallArg(call->arg, ctx, rctx);
            if (!value) return std::nullopt;
            if (call->name == "encode") return urlEncode(*value);
            if (call->name == "decode") return urlDecode(*value);
            return std::nullopt;
        }
        // Unrecognised function-form builtin — fall through. The caller
        // surfaces it as unresolved, which is what we want.
        return std::nullopt;
    }

    // Split off optional ±offset before doing anything else, so the
    // builtin matcher works on the bare name.
    std::chrono::seconds offset{0};
    bool hasOffset = false;
    {
        // Find the rightmost '+' or '-' that is *not* inside parentheses.
        // Keeps the door open for $.fn(arg-with-dash) without misparsing.
        int depth = 0;
        std::size_t opPos = std::string_view::npos;
        char opCh = '\0';
        for (std::size_t i = 0; i < ref.size(); ++i) {
            const char c = ref[i];
            if (c == '(') ++depth;
            else if (c == ')') --depth;
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
        if (hasOffset) return std::nullopt;  // offsets only apply to $.now
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
            // RNG seeded per call is deliberately weak: faker is for fixture
            // data, not security. Matches the prior behaviour.
            return "+1555" + std::to_string(std::random_device{}() % 10000000);
        }
        return std::string{"faker_"} + std::string{fakerType} + "_" +
               generateUuid().substr(0, 8);
    }

    return std::nullopt;
}

/// Resolve dotted refs (env.X, secret.X, actor.var, resource.var) and
/// indexed refs (resource[N].var). PRD §5.7 categories 2–5.
ResolvedRef resolveDotted(std::string_view ref,
                          const RunContext& ctx,
                          const ResolveContext& rctx) {
    const auto dotPos = ref.find('.');
    if (dotPos == std::string_view::npos) return std::nullopt;

    const auto scope = std::string{ref.substr(0, dotPos)};
    const auto field = std::string{ref.substr(dotPos + 1)};

    // 4. Environment variables (env.X)
    if (scope == "env") {
        auto it = rctx.envVars.find(field);
        if (it != rctx.envVars.end()) return it->second;
        return std::nullopt;
    }

    // 5. Secrets (secret.X)
    if (scope == "secret") {
        auto it = rctx.secrets.find(field);
        if (it != rctx.secrets.end()) return it->second;
        return std::nullopt;
    }

    // 2. Actor sessions
    if (auto* session = ctx.session(ActorId{scope}); session != nullptr) {
        auto it = session->variables.find(field);
        if (it != session->variables.end()) return it->second;
    }

    // 3. Resource extractions
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

    // Search instances in reverse (most recent first) for the field.
    // This handles the common case where a single logical resource
    // accrues fields across multiple operations (order.create extracts
    // order_id; order.pay extracts payment_id; later we want
    // order.order_id to still resolve from the older instance).
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

VariableResolver::Result VariableResolver::resolve(
    std::string_view templateStr,
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
            output += "{{" + trimmed + "}}";  // Leave unresolved in place.
        }

        lastPos = matchPos + static_cast<std::size_t>(it->length());
    }

    output += input.substr(lastPos);
    return Result{std::move(output), std::move(unresolved)};
}

}  // namespace chainapi::engine
