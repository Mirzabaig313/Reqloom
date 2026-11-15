// QuickJsHookRunner — sandboxed JS execution via quickjs-ng.
//
// One JSRuntime/JSContext is created per hook invocation and torn down on
// return. That's not the fastest model, but it's the simplest and the
// safest: every hook runs in a clean slate with no carry-over of globals,
// closures, or module bindings between invocations.
//
// The JS surface exposed on `ctx` is intentionally narrow:
//
//   ctx.request.{method,url,headers,body}      — read/write (pre+post)
//   ctx.response.{status,headers,body}         — read/write (post only)
//   ctx.actors[name][var]                      — read-only, the operation's own actor only
//   ctx.env[name]                              — read-only
//   ctx.secret[name]                           — read-only, only the names the engine passes
//
// A body or header value that is not valid UTF-8 is exposed as a binary string: one
// character per byte (U+0000–U+00FF). Written back, a string whose characters are all
// ≤ U+00FF becomes those bytes; any other string is written as UTF-8. A field the hook
// leaves alone comes back byte-for-byte, NUL bytes included.
//   ctx.base64.{encode,decode}(s)              — string codecs
//   ctx.hex.{encode,decode}(s)
//   ctx.url.{encode,decode}(s)
//   ctx.hmac.{sha1,sha256,sha512}(key, msg)    — returns hex
//   ctx.hash.{md5,sha1,sha256,sha512}(msg)     — returns hex (md5/sha1
//                                                 surface deferred — no
//                                                 op-level need yet)
//   ctx.jwt.sign(claims, key, alg)             — HS256/HS512
//   ctx.json.{stringify,parse}                 — canonicalised stringify
//                                                 (sorted keys)
//
// Sandbox guarantees:
//   - No filesystem access (QuickJS doesn't expose `os` / `std` unless we
//     opt in with `js_init_module_*`; we don't).
//   - No network (same — we don't add `fetch`/`XMLHttpRequest`).
//   - No `require` (CommonJS isn't built in, and we don't supply a loader).
//   - 1-second wall-clock interrupt budget per hook invocation. Hooks
//     longer than that fail with `E_HOOK_TIMEOUT`.
//   - 8 MiB stack cap to avoid runaway recursion bringing the process
//     down via stack overflow.
//   - 64 MiB heap cap; exceeding it fails the hook ("out of memory").
//   - Async hooks: pending promise jobs are drained within the same budget
//     and a returned promise must settle (rejection fails the hook).
//
// The runner accepts BOTH module-style (`export default function (ctx){…}`)
// and script-style (`(ctx) => {…}`, or a plain top-level body that mutates
// `ctx`) hooks. Module mode is detected by the presence of `export ` /
// `import ` at the top level.

#include "QuickJsHookRunner.h"

#include "../../domain/Codecs.h"
#include "../util/Crypto.h"

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wold-style-cast"
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wconversion"
#endif

#include <quickjs.h>

#ifdef __clang__
#pragma clang diagnostic pop
#endif

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace reqloom::engine {

namespace {

using namespace codecs;

// ─── Wall-clock budget enforcement ───────────────────────────────────────────

struct InterruptState {
    std::chrono::steady_clock::time_point deadline;
    /// Set when the budget ran out and execution was interrupted. The
    /// timeout verdict comes from this flag, never from exception text.
    bool fired{false};
};

int interruptCallback(JSRuntime* /*rt*/, void* opaque) {
    auto* state = static_cast<InterruptState*>(opaque);
    if (std::chrono::steady_clock::now() >= state->deadline) {
        state->fired = true;
        return 1;
    }
    return 0;
}

// ─── Tiny RAII wrappers around JS lifetime ───────────────────────────────────

struct JsRuntimeDeleter {
    void operator()(JSRuntime* rt) const noexcept {
        if (rt != nullptr) {
            JS_FreeRuntime(rt);
        }
    }
};
using JsRuntimePtr = std::unique_ptr<JSRuntime, JsRuntimeDeleter>;

struct JsContextDeleter {
    void operator()(JSContext* ctx) const noexcept {
        if (ctx != nullptr) {
            JS_FreeContext(ctx);
        }
    }
};
using JsContextPtr = std::unique_ptr<JSContext, JsContextDeleter>;

class JsValueGuard {
public:
    JsValueGuard(JSContext* ctx, JSValue v) : ctx_(ctx), v_(v) {}
    ~JsValueGuard() { JS_FreeValue(ctx_, v_); }
    JsValueGuard(const JsValueGuard&) = delete;
    JsValueGuard& operator=(const JsValueGuard&) = delete;
    JsValueGuard(JsValueGuard&&) = delete;
    JsValueGuard& operator=(JsValueGuard&&) = delete;
    [[nodiscard]] JSValue value() const noexcept { return v_; }

private:
    JSContext* ctx_;
    JSValue v_;
};

// ─── String marshalling ──────────────────────────────────────────────────────

[[nodiscard]] std::string toCppString(JSContext* ctx, JSValueConst v) {
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        return {};
    }
    std::size_t len{0};
    const char* c = JS_ToCStringLen(ctx, &len, v);
    if (c == nullptr) {
        return {};
    }
    std::string out{c, len};
    JS_FreeCString(ctx, c);
    return out;
}

[[nodiscard]] JSValue jsStr(JSContext* ctx, std::string_view s) {
    return JS_NewStringLen(ctx, s.data(), s.size());
}

// Strict UTF-8 (no overlongs, surrogates or code points above U+10FFFF), O(n).
[[nodiscard]] bool isValidUtf8(std::string_view s) noexcept {
    std::size_t i{0};
    while (i < s.size()) {
        const auto lead = static_cast<unsigned char>(s[i]);
        std::size_t extra{0};
        std::uint32_t cp{0};
        if (lead < 0x80U) {
            ++i;
            continue;
        }
        if ((lead & 0xE0U) == 0xC0U) {
            extra = 1;
            cp = lead & 0x1FU;
        } else if ((lead & 0xF0U) == 0xE0U) {
            extra = 2;
            cp = lead & 0x0FU;
        } else if ((lead & 0xF8U) == 0xF0U) {
            extra = 3;
            cp = lead & 0x07U;
        } else {
            return false;
        }
        if (i + extra >= s.size()) {
            return false;
        }
        for (std::size_t k = 1; k <= extra; ++k) {
            const auto cont = static_cast<unsigned char>(s[i + k]);
            if ((cont & 0xC0U) != 0x80U) {
                return false;
            }
            cp = (cp << 6U) | (cont & 0x3FU);
        }
        constexpr std::array<std::uint32_t, 4> kMinForLength{0, 0x80, 0x800, 0x10000};
        if (cp < kMinForLength[extra] || cp > 0x10FFFFU || (cp >= 0xD800U && cp <= 0xDFFFU)) {
            return false;
        }
        i += extra + 1;
    }
    return true;
}

/// A byte string as a JS string: UTF-8 text as text, anything else one character per byte.
[[nodiscard]] JSValue jsBytes(JSContext* ctx, std::string_view s) {
    if (isValidUtf8(s)) {
        return jsStr(ctx, s);
    }
    std::vector<std::uint16_t> units(s.begin(), s.end());
    for (auto& unit : units) {
        unit = static_cast<std::uint16_t>(unit & 0xFFU);
    }
    return JS_NewStringUTF16(ctx, units.data(), units.size());
}

/// Inverse of `jsBytes` for a value whose original was `binary`.
[[nodiscard]] std::string fromJsBytes(JSContext* ctx, JSValueConst v, bool binary) {
    if (!binary) {
        return toCppString(ctx, v);
    }
    std::size_t len{0};
    const std::uint16_t* units = JS_ToCStringLenUTF16(ctx, &len, v);
    if (units == nullptr) {
        return {};
    }
    std::string out;
    out.reserve(len);
    bool allBytes{true};
    for (std::size_t i = 0; i < len; ++i) {
        if (units[i] > 0xFFU) {
            allBytes = false;
            break;
        }
        out.push_back(static_cast<char>(units[i]));
    }
    JS_FreeCStringUTF16(ctx, units);
    return allBytes ? out : toCppString(ctx, v);
}

void setStringProp(JSContext* ctx, JSValueConst obj, const char* name, std::string_view value) {
    JS_SetPropertyStr(ctx, obj, name, jsStr(ctx, value));
}

void setIntProp(JSContext* ctx, JSValueConst obj, const char* name, int32_t value) {
    JS_SetPropertyStr(ctx, obj, name, JS_NewInt32(ctx, value));
}

[[nodiscard]] JSValue makeStringMap(JSContext* ctx, const std::map<std::string, std::string>& map) {
    JSValue obj = JS_NewObject(ctx);
    for (const auto& [k, v] : map) {
        JS_SetPropertyStr(ctx, obj, k.c_str(), jsBytes(ctx, v));
    }
    return obj;
}

/// Read a JS string map back; `original` decides how each pre-existing value is decoded.
[[nodiscard]] std::map<std::string, std::string> readStringMap(
    JSContext* ctx, JSValueConst obj, const std::map<std::string, std::string>& original) {
    std::map<std::string, std::string> out;
    if (!JS_IsObject(obj)) {
        return out;
    }

    JSPropertyEnum* tab = nullptr;
    uint32_t len = 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, obj, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) != 0) {
        return out;
    }
    for (uint32_t i = 0; i < len; ++i) {
        const char* key = JS_AtomToCString(ctx, tab[i].atom);
        if (key == nullptr) {
            continue;
        }

        JSValue const v = JS_GetProperty(ctx, obj, tab[i].atom);

        if (JS_IsString(v)) {
            const auto before = original.find(key);
            const bool binary = before != original.end() && !isValidUtf8(before->second);
            out.emplace(key, fromJsBytes(ctx, v, binary));
        }
        JS_FreeValue(ctx, v);
        JS_FreeCString(ctx, key);
    }
    JS_FreePropertyEnum(ctx, tab, len);
    return out;
}

// ─── Helper bindings ─────────────────────────────────────────────────────────
//
// All helpers follow a common shape:
//   - First arg is always present, otherwise return undefined
//   - Result is always a JS string (callers' templates are textual)
//   - On failure (malformed input, missing arg) we return undefined; the
//     hook author can detect that and choose how to react

[[nodiscard]] std::optional<std::string> argString(JSContext* ctx,
                                                   int argc,
                                                   JSValueConst* argv,
                                                   int idx) {
    if (idx >= argc) {
        return std::nullopt;
    }
    if (!JS_IsString(argv[idx])) {
        return std::nullopt;
    }
    return toCppString(ctx, argv[idx]);
}

JSValue jsBase64Encode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto s = argString(ctx, argc, argv, 0);
    if (!s) {
        return JS_UNDEFINED;
    }
    return jsStr(ctx, base64Encode(*s));
}

JSValue jsBase64Decode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto s = argString(ctx, argc, argv, 0);
    if (!s) {
        return JS_UNDEFINED;
    }
    auto decoded = base64Decode(*s);
    if (!decoded) {
        return JS_UNDEFINED;
    }
    return jsStr(ctx, *decoded);
}

JSValue jsHexEncode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto s = argString(ctx, argc, argv, 0);
    if (!s) {
        return JS_UNDEFINED;
    }
    return jsStr(ctx, hexEncode(*s));
}

JSValue jsHexDecode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto s = argString(ctx, argc, argv, 0);
    if (!s) {
        return JS_UNDEFINED;
    }
    auto decoded = hexDecode(*s);
    if (!decoded) {
        return JS_UNDEFINED;
    }
    return jsStr(ctx, *decoded);
}

JSValue jsUrlEncode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto s = argString(ctx, argc, argv, 0);
    if (!s) {
        return JS_UNDEFINED;
    }
    return jsStr(ctx, urlEncode(*s));
}

JSValue jsUrlDecode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto s = argString(ctx, argc, argv, 0);
    if (!s) {
        return JS_UNDEFINED;
    }
    auto decoded = urlDecode(*s);
    if (!decoded) {
        return JS_UNDEFINED;
    }
    return jsStr(ctx, *decoded);
}

template <std::string (*Fn)(std::string_view, std::string_view)>
JSValue jsHmac(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto key = argString(ctx, argc, argv, 0);
    auto msg = argString(ctx, argc, argv, 1);
    if (!key || !msg) {
        return JS_UNDEFINED;
    }
    return jsStr(ctx, hexEncode(Fn(*key, *msg)));
}

JSValue jsSha256(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto msg = argString(ctx, argc, argv, 0);
    if (!msg) {
        return JS_UNDEFINED;
    }
    return jsStr(ctx, hexEncode(crypto::sha256(*msg)));
}

/// JWT signing entry point. Signature: `ctx.jwt.sign(claims, key, alg?)`.
/// `claims` may be either a JS object or a JSON string. `alg` defaults
/// to "HS256"; "HS512" is also supported.
JSValue jsJwtSign(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) {
        return JS_UNDEFINED;
    }

    std::string payloadJson;
    if (JS_IsString(argv[0])) {
        payloadJson = toCppString(ctx, argv[0]);
    } else if (JS_IsObject(argv[0])) {
        // Stringify via the global JSON.stringify so we don't reimplement
        // canonicalisation. The user's expectation is "JS object → JWT
        // payload", which matches plain stringify semantics.
        JSValue const global = JS_GetGlobalObject(ctx);
        JsValueGuard const globalGuard{ctx, global};
        JSValue const jsonObj = JS_GetPropertyStr(ctx, global, "JSON");
        JsValueGuard const jsonGuard{ctx, jsonObj};
        JSValue const stringify = JS_GetPropertyStr(ctx, jsonObj, "stringify");
        JsValueGuard const stringifyGuard{ctx, stringify};
        JSValue args[1] = {argv[0]};
        JSValue const str = JS_Call(ctx, stringify, jsonObj, 1, args);
        JsValueGuard const strGuard{ctx, str};
        if (JS_IsException(str)) {
            return JS_UNDEFINED;
        }
        payloadJson = toCppString(ctx, str);
    } else {
        return JS_UNDEFINED;
    }

    if (!JS_IsString(argv[1])) {
        return JS_UNDEFINED;
    }
    const auto key = toCppString(ctx, argv[1]);

    std::string alg = "HS256";
    if (argc >= 3 && JS_IsString(argv[2])) {
        alg = toCppString(ctx, argv[2]);
    }

    std::string signed_;
    if (alg == "HS256") {
        signed_ = crypto::jwtSignHs256(payloadJson, key);
    } else if (alg == "HS512") {
        signed_ = crypto::jwtSignHs512(payloadJson, key);
    } else {
        return JS_UNDEFINED;
    }
    if (signed_.empty()) {
        return JS_UNDEFINED;
    }
    return jsStr(ctx, signed_);
}

// QuickJS calls helper functions from its own C stack frames. A C++
// exception (e.g. std::bad_alloc from the codec/crypto/std::string calls
// the helpers make) unwinding through those frames is undefined behaviour.
// Every helper is registered through this noexcept trampoline, which
// collapses any throw into the helpers' existing "return undefined on
// failure" contract. Routing registration through installHelper makes the
// guard impossible to forget for a newly added helper.
template <JSCFunction* Fn>
JSValue guardHelper(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) noexcept {
    try {
        return Fn(ctx, thisVal, argc, argv);
    } catch (...) {
        return JS_UNDEFINED;
    }
}

template <JSCFunction* Fn>
void installHelper(JSContext* ctx, JSValueConst parent, const char* name, int arity) {
    JSValue const f = JS_NewCFunction(ctx, &guardHelper<Fn>, name, arity);
    JS_SetPropertyStr(ctx, parent, name, f);
}

// ─── Build the `ctx` object handed to the hook ───────────────────────────────

[[nodiscard]] JSValue buildHookCtx(JSContext* ctx, const HookContext& hctx, bool includeResponse) {
    JSValue root = JS_NewObject(ctx);

    // request
    {
        JSValue const req = JS_NewObject(ctx);
        setStringProp(ctx, req, "method", std::string{codecs::methodToString(hctx.request.method)});
        setStringProp(ctx, req, "url", hctx.request.url);
        JS_SetPropertyStr(ctx, req, "headers", makeStringMap(ctx, hctx.request.headers));
        if (hctx.request.body) {
            JS_SetPropertyStr(ctx, req, "body", jsBytes(ctx, *hctx.request.body));
        } else {
            JS_SetPropertyStr(ctx, req, "body", JS_NULL);
        }
        JS_SetPropertyStr(ctx, root, "request", req);
    }

    // response (post hooks only)
    if (includeResponse && hctx.response) {
        JSValue const resp = JS_NewObject(ctx);
        setIntProp(ctx, resp, "status", hctx.response->status);
        JS_SetPropertyStr(ctx, resp, "headers", makeStringMap(ctx, hctx.response->headers));
        JS_SetPropertyStr(ctx, resp, "body", jsBytes(ctx, hctx.response->body));
        JS_SetPropertyStr(ctx, root, "response", resp);
    } else {
        JS_SetPropertyStr(ctx, root, "response", JS_NULL);
    }

    // actors → { actorName: { var: value, ... }, ... }
    {
        JSValue const actors = JS_NewObject(ctx);
        for (const auto& [actor, vars] : hctx.variables) {
            JS_SetPropertyStr(ctx, actors, actor.c_str(), makeStringMap(ctx, vars));
        }
        JS_SetPropertyStr(ctx, root, "actors", actors);
    }

    JS_SetPropertyStr(ctx, root, "env", makeStringMap(ctx, hctx.env));
    JS_SetPropertyStr(ctx, root, "secret", makeStringMap(ctx, hctx.secrets));

    // helpers
    {
        JSValue const base64 = JS_NewObject(ctx);
        installHelper<jsBase64Encode>(ctx, base64, "encode", 1);
        installHelper<jsBase64Decode>(ctx, base64, "decode", 1);
        JS_SetPropertyStr(ctx, root, "base64", base64);
    }
    {
        JSValue const hex = JS_NewObject(ctx);
        installHelper<jsHexEncode>(ctx, hex, "encode", 1);
        installHelper<jsHexDecode>(ctx, hex, "decode", 1);
        JS_SetPropertyStr(ctx, root, "hex", hex);
    }
    {
        JSValue const url = JS_NewObject(ctx);
        installHelper<jsUrlEncode>(ctx, url, "encode", 1);
        installHelper<jsUrlDecode>(ctx, url, "decode", 1);
        JS_SetPropertyStr(ctx, root, "url", url);
    }
    {
        JSValue const hmac = JS_NewObject(ctx);
        installHelper<jsHmac<crypto::hmacSha1>>(ctx, hmac, "sha1", 2);
        installHelper<jsHmac<crypto::hmacSha256>>(ctx, hmac, "sha256", 2);
        installHelper<jsHmac<crypto::hmacSha512>>(ctx, hmac, "sha512", 2);
        JS_SetPropertyStr(ctx, root, "hmac", hmac);
    }
    {
        JSValue const hash = JS_NewObject(ctx);
        installHelper<jsSha256>(ctx, hash, "sha256", 1);
        JS_SetPropertyStr(ctx, root, "hash", hash);
    }
    {
        JSValue const jwt = JS_NewObject(ctx);
        installHelper<jsJwtSign>(ctx, jwt, "sign", 3);
        JS_SetPropertyStr(ctx, root, "jwt", jwt);
    }

    return root;
}

// ─── Read mutated request/response back from JS ──────────────────────────────

void readMutatedRequest(JSContext* ctx, JSValueConst root, HookRequestView& outReq) {
    JSValue const req = JS_GetPropertyStr(ctx, root, "request");
    JsValueGuard const guard{ctx, req};
    if (!JS_IsObject(req)) {
        return;
    }

    {
        JSValue const url = JS_GetPropertyStr(ctx, req, "url");
        JsValueGuard const urlGuard{ctx, url};
        if (JS_IsString(url)) {
            outReq.url = toCppString(ctx, url);
        }
    }
    {
        JSValue const body = JS_GetPropertyStr(ctx, req, "body");
        JsValueGuard const bodyGuard{ctx, body};
        if (JS_IsString(body)) {
            outReq.body = fromJsBytes(ctx, body, outReq.body && !isValidUtf8(*outReq.body));
        } else if (JS_IsNull(body) || JS_IsUndefined(body)) {
            outReq.body = std::nullopt;
        }
    }
    {
        JSValue const headers = JS_GetPropertyStr(ctx, req, "headers");
        JsValueGuard const headersGuard{ctx, headers};
        if (JS_IsObject(headers)) {
            outReq.headers = readStringMap(ctx, headers, outReq.headers);
        }
    }
    // method is intentionally read-only post-resolution; templates have
    // already locked it in by the time hooks see ctx.
}

void readMutatedResponse(JSContext* ctx, JSValueConst root, HookResponseView& outResp) {
    JSValue const resp = JS_GetPropertyStr(ctx, root, "response");
    JsValueGuard const guard{ctx, resp};
    if (!JS_IsObject(resp)) {
        return;
    }

    {
        JSValue const status = JS_GetPropertyStr(ctx, resp, "status");
        JsValueGuard const statusGuard{ctx, status};
        if (JS_IsNumber(status)) {
            int32_t s = 0;
            if (JS_ToInt32(ctx, &s, status) == 0) {
                outResp.status = s;
            }
        }
    }
    {
        JSValue const body = JS_GetPropertyStr(ctx, resp, "body");
        JsValueGuard const bodyGuard{ctx, body};
        if (JS_IsString(body)) {
            outResp.body = fromJsBytes(ctx, body, !isValidUtf8(outResp.body));
        }
    }
    {
        JSValue const headers = JS_GetPropertyStr(ctx, resp, "headers");
        JsValueGuard const headersGuard{ctx, headers};
        if (JS_IsObject(headers)) {
            outResp.headers = readStringMap(ctx, headers, outResp.headers);
        }
    }
}

// ─── Error formatting ────────────────────────────────────────────────────────

[[nodiscard]] std::string formatJsValue(JSContext* ctx, JSValueConst ex) {
    if (JS_IsNull(ex) || JS_IsUndefined(ex)) {
        return "<unknown>";
    }

    auto base = toCppString(ctx, ex);
    JSValue const stack = JS_GetPropertyStr(ctx, ex, "stack");
    JsValueGuard const stackGuard{ctx, stack};
    if (JS_IsString(stack)) {
        auto stackStr = toCppString(ctx, stack);
        if (!stackStr.empty()) {
            base += "\n" + stackStr;
        }
    }
    return base;
}

[[nodiscard]] std::string formatJsException(JSContext* ctx) {
    JSValue const ex = JS_GetException(ctx);
    JsValueGuard const guard{ctx, ex};
    return formatJsValue(ctx, ex);
}

// ─── Script shape detection ──────────────────────────────────────────────────
//
// Two surfaces from the user's perspective:
//
//   (a) A module file: `export default function (ctx) { ... }` — the
//       schema parser loads sibling .js files in this style. We compile
//       it as JS_EVAL_TYPE_MODULE, retrieve the default export, and call
//       it with `ctx` as the only argument.
//
//   (b) An inline `pre_request: |` block: a sequence of statements that
//       reference a free `ctx` symbol. We wrap that body in
//       `(function (ctx) { <body> })` and call the wrapper.
//
// Heuristic: if the script contains a top-level `export ` or `import `
// keyword, treat it as (a). Otherwise (b).

[[nodiscard]] bool looksLikeModule(std::string_view script) {
    while (!script.empty() && (std::isspace(static_cast<unsigned char>(script.front())) != 0)) {
        script.remove_prefix(1);
    }
    return script.starts_with("export ") || script.starts_with("import ");
}

}  // namespace

// ─── Public surface ──────────────────────────────────────────────────────────

QuickJsHookRunner::QuickJsHookRunner() = default;
QuickJsHookRunner::~QuickJsHookRunner() = default;

namespace {

constexpr auto kHookTimeout = std::chrono::seconds(1);
constexpr std::size_t kStackSize = std::size_t{8} * 1024 * 1024;     // 8 MiB
constexpr std::size_t kMemoryLimit = std::size_t{64} * 1024 * 1024;  // 64 MiB

using HookResult = std::expected<void, ReqloomError>;

[[nodiscard]] std::unexpected<ReqloomError> hookError(ErrorCode code, std::string detail) {
    return std::unexpected(ReqloomError{code, ErrorClass::Hook, std::move(detail)});
}

// Timeout iff the interrupt handler fired; out-of-memory surfaces as QuickJS's "out of memory".
[[nodiscard]] std::unexpected<ReqloomError> exceptionError(JSContext* c,
                                                           const InterruptState& ist,
                                                           std::string_view prefix) {
    const auto err = formatJsException(c);
    if (ist.fired) {
        return hookError(ErrorCode::HookTimeout, "hook exceeded 1s budget");
    }
    return hookError(ErrorCode::HookFailure, std::string{prefix} + err);
}

// Run queued promise jobs under the same deadline and memory limit, then judge `value`: a
// rejected promise fails with its reason. The sandbox has no timers or I/O, so a promise
// still pending after the job queue drains can never settle.
[[nodiscard]] HookResult settle(JSContext* c,
                                const InterruptState& ist,
                                JSValueConst value,
                                std::string_view what) {
    JSRuntime* rt = JS_GetRuntime(c);
    for (;;) {
        JSContext* jobCtx = nullptr;
        const int r = JS_ExecutePendingJob(rt, &jobCtx);
        if (r == 0) {
            break;
        }
        if (r < 0) {
            return exceptionError(jobCtx != nullptr ? jobCtx : c, ist, std::string{what} + ": ");
        }
    }
    if (!JS_IsPromise(value)) {
        return {};
    }
    switch (JS_PromiseState(c, value)) {
        case JS_PROMISE_REJECTED: {
            JSValue const reason = JS_PromiseResult(c, value);
            JsValueGuard const reasonGuard{c, reason};
            return hookError(ErrorCode::HookFailure,
                             std::string{what} + " rejected: " + formatJsValue(c, reason));
        }
        case JS_PROMISE_PENDING:
            if (ist.fired) {
                return hookError(ErrorCode::HookTimeout, "hook exceeded 1s budget");
            }
            return hookError(ErrorCode::HookFailure,
                             std::string{what} +
                                 " returned a promise that never settled (no timers or I/O "
                                 "exist in the hook sandbox)");
        default:
            return {};
    }
}

[[nodiscard]] std::expected<HookOutcome, ReqloomError> runScript(const std::string& script,
                                                                 HookContext context,
                                                                 bool includeResponse) {
    JsRuntimePtr const rt{JS_NewRuntime()};
    if (!rt) {
        return std::unexpected(
            ReqloomError{ErrorCode::HookFailure, ErrorClass::Hook, "JS_NewRuntime returned null"});
    }
    JS_SetMaxStackSize(rt.get(), kStackSize);
    JS_SetMemoryLimit(rt.get(), kMemoryLimit);

    InterruptState ist{std::chrono::steady_clock::now() + kHookTimeout};
    JS_SetInterruptHandler(rt.get(), interruptCallback, &ist);

    JsContextPtr const jsctx{JS_NewContext(rt.get())};
    if (!jsctx) {
        return std::unexpected(
            ReqloomError{ErrorCode::HookFailure, ErrorClass::Hook, "JS_NewContext returned null"});
    }

    JSContext* c = jsctx.get();

    // Build the `ctx` object and stash it on the global as `__reqloom_ctx`
    // so wrapper code can reach it whether the script is a module or an
    // inline body.
    JSValue const hookCtxObj = buildHookCtx(c, context, includeResponse);
    JSValue const global = JS_GetGlobalObject(c);
    JsValueGuard const globalGuard{c, global};
    JS_SetPropertyStr(c, global, "__reqloom_ctx", JS_DupValue(c, hookCtxObj));
    JsValueGuard const hookCtxGuard{c, hookCtxObj};

    auto fail = [&](ErrorCode code, std::string detail) {
        return std::unexpected(ReqloomError{code, ErrorClass::Hook, std::move(detail)});
    };

    if (looksLikeModule(script)) {
        // Compile the module and trigger evaluation. The user's default
        // export is invoked via a thin script that imports it back.
        JSValue const compiled = JS_Eval(c,
                                         script.data(),
                                         script.size(),
                                         "hook.mjs",
                                         JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(compiled)) {
            JS_FreeValue(c, compiled);
            return fail(ErrorCode::HookFailure, "hook compile failed: " + formatJsException(c));
        }
        JSValue const evalOk = JS_EvalFunction(c, compiled);
        JsValueGuard const evalOkGuard{c, evalOk};
        if (JS_IsException(evalOk)) {
            return exceptionError(c, ist, "hook failed: ");
        }
        if (auto settled = settle(c, ist, evalOk, "hook module evaluation"); !settled) {
            return std::unexpected(std::move(settled.error()));
        }

        // QuickJS are cached by name.
        const std::string driver =
            "import fn from 'hook.mjs';\n"
            "globalThis.__reqloom_invoke = () => fn(globalThis.__reqloom_ctx);\n";
        JSValue const driverEval =
            JS_Eval(c, driver.data(), driver.size(), "<driver>", JS_EVAL_TYPE_MODULE);
        JsValueGuard const driverGuard{c, driverEval};
        if (JS_IsException(driverEval)) {
            return exceptionError(c, ist, "hook driver eval failed: ");
        }
        if (auto settled = settle(c, ist, driverEval, "hook driver"); !settled) {
            return std::unexpected(std::move(settled.error()));
        }

        JSValue const invoke = JS_GetPropertyStr(c, global, "__reqloom_invoke");
        JsValueGuard const invokeGuard{c, invoke};
        if (!JS_IsFunction(c, invoke)) {
            return fail(ErrorCode::HookFailure, "module did not export a default function");
        }
        JSValue const result = JS_Call(c, invoke, JS_UNDEFINED, 0, nullptr);
        JsValueGuard const resultGuard{c, result};
        if (JS_IsException(result)) {
            return exceptionError(c, ist, "hook threw: ");
        }
        if (auto settled = settle(c, ist, result, "hook"); !settled) {
            return std::unexpected(std::move(settled.error()));
        }
    } else {
        // Inline body. Wrap in `(function(ctx){ <body> })(__reqloom_ctx)`.
        // We don't add `'use strict';` because hook bodies are user code
        // and forcing strict mode would silently break valid loose-mode
        // snippets. Crypto helpers don't care either way.
        const std::string wrapped =
            "(function(ctx){\n" + script + "\n}).call(undefined, globalThis.__reqloom_ctx);\n";
        JSValue const evalRes =
            JS_Eval(c, wrapped.data(), wrapped.size(), "hook.js", JS_EVAL_TYPE_GLOBAL);
        JsValueGuard const evalGuard{c, evalRes};
        if (JS_IsException(evalRes)) {
            return exceptionError(c, ist, "hook threw: ");
        }
        if (auto settled = settle(c, ist, evalRes, "hook"); !settled) {
            return std::unexpected(std::move(settled.error()));
        }
    }

    HookOutcome out;
    out.mutatedRequest = context.request;
    readMutatedRequest(c, hookCtxObj, out.mutatedRequest);
    if (includeResponse && context.response) {
        out.mutatedResponse = *context.response;
        readMutatedResponse(c, hookCtxObj, *out.mutatedResponse);
    }
    return out;
}

}  // namespace

std::expected<HookOutcome, ReqloomError> QuickJsHookRunner::runPreRequest(const std::string& script,
                                                                          HookContext context) {
    return runScript(script, std::move(context), /*includeResponse=*/false);
}

std::expected<HookOutcome, ReqloomError> QuickJsHookRunner::runPostResponse(
    const std::string& script, HookContext context) {
    return runScript(script, std::move(context), /*includeResponse=*/true);
}

}  // namespace reqloom::engine
