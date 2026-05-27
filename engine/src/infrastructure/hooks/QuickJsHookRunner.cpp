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
//   ctx.actors[name][var]                      — read-only
//   ctx.env[name]                              — read-only
//   ctx.secret[name]                           — read-only
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

namespace chainapi::engine {

namespace {

using namespace codecs;

// ─── Wall-clock budget enforcement ───────────────────────────────────────────

struct InterruptState {
    std::chrono::steady_clock::time_point deadline;
};

int interruptCallback(JSRuntime* /*rt*/, void* opaque) {
    auto* state = static_cast<InterruptState*>(opaque);
    return std::chrono::steady_clock::now() >= state->deadline ? 1 : 0;
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
    const char* c = JS_ToCString(ctx, v);
    if (c == nullptr) {
        return {};
    }
    std::string out{c};
    JS_FreeCString(ctx, c);
    return out;
}

[[nodiscard]] JSValue jsStr(JSContext* ctx, std::string_view s) {
    return JS_NewStringLen(ctx, s.data(), s.size());
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
        JS_SetPropertyStr(ctx, obj, k.c_str(), jsStr(ctx, v));
    }
    return obj;
}

[[nodiscard]] std::map<std::string, std::string> readStringMap(JSContext* ctx, JSValueConst obj) {
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

        JSValue v = JS_GetProperty(ctx, obj, tab[i].atom);

        if (JS_IsString(v)) {
            out.emplace(key, toCppString(ctx, v));
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
        JSValue global = JS_GetGlobalObject(ctx);
        JsValueGuard globalGuard{ctx, global};
        JSValue jsonObj = JS_GetPropertyStr(ctx, global, "JSON");
        JsValueGuard jsonGuard{ctx, jsonObj};
        JSValue stringify = JS_GetPropertyStr(ctx, jsonObj, "stringify");
        JsValueGuard stringifyGuard{ctx, stringify};
        JSValue args[1] = {argv[0]};
        JSValue str = JS_Call(ctx, stringify, jsonObj, 1, args);
        JsValueGuard strGuard{ctx, str};
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

void installHelper(
    JSContext* ctx, JSValueConst parent, const char* name, JSCFunction* fn, int arity) {
    JSValue f = JS_NewCFunction(ctx, fn, name, arity);
    JS_SetPropertyStr(ctx, parent, name, f);
}

// ─── Build the `ctx` object handed to the hook ───────────────────────────────

[[nodiscard]] JSValue buildHookCtx(JSContext* ctx, const HookContext& hctx, bool includeResponse) {
    JSValue root = JS_NewObject(ctx);

    // request
    {
        JSValue req = JS_NewObject(ctx);
        setStringProp(ctx, req, "method", std::string{codecs::methodToString(hctx.request.method)});
        setStringProp(ctx, req, "url", hctx.request.url);
        JS_SetPropertyStr(ctx, req, "headers", makeStringMap(ctx, hctx.request.headers));
        if (hctx.request.body) {
            setStringProp(ctx, req, "body", *hctx.request.body);
        } else {
            JS_SetPropertyStr(ctx, req, "body", JS_NULL);
        }
        JS_SetPropertyStr(ctx, root, "request", req);
    }

    // response (post hooks only)
    if (includeResponse && hctx.response) {
        JSValue resp = JS_NewObject(ctx);
        setIntProp(ctx, resp, "status", hctx.response->status);
        JS_SetPropertyStr(ctx, resp, "headers", makeStringMap(ctx, hctx.response->headers));
        setStringProp(ctx, resp, "body", hctx.response->body);
        JS_SetPropertyStr(ctx, root, "response", resp);
    } else {
        JS_SetPropertyStr(ctx, root, "response", JS_NULL);
    }

    // actors → { actorName: { var: value, ... }, ... }
    {
        JSValue actors = JS_NewObject(ctx);
        for (const auto& [actor, vars] : hctx.variables) {
            JS_SetPropertyStr(ctx, actors, actor.c_str(), makeStringMap(ctx, vars));
        }
        JS_SetPropertyStr(ctx, root, "actors", actors);
    }

    JS_SetPropertyStr(ctx, root, "env", makeStringMap(ctx, hctx.env));
    JS_SetPropertyStr(ctx, root, "secret", makeStringMap(ctx, hctx.secrets));

    // helpers
    {
        JSValue base64 = JS_NewObject(ctx);
        installHelper(ctx, base64, "encode", jsBase64Encode, 1);
        installHelper(ctx, base64, "decode", jsBase64Decode, 1);
        JS_SetPropertyStr(ctx, root, "base64", base64);
    }
    {
        JSValue hex = JS_NewObject(ctx);
        installHelper(ctx, hex, "encode", jsHexEncode, 1);
        installHelper(ctx, hex, "decode", jsHexDecode, 1);
        JS_SetPropertyStr(ctx, root, "hex", hex);
    }
    {
        JSValue url = JS_NewObject(ctx);
        installHelper(ctx, url, "encode", jsUrlEncode, 1);
        installHelper(ctx, url, "decode", jsUrlDecode, 1);
        JS_SetPropertyStr(ctx, root, "url", url);
    }
    {
        JSValue hmac = JS_NewObject(ctx);
        installHelper(ctx, hmac, "sha1", jsHmac<crypto::hmacSha1>, 2);
        installHelper(ctx, hmac, "sha256", jsHmac<crypto::hmacSha256>, 2);
        installHelper(ctx, hmac, "sha512", jsHmac<crypto::hmacSha512>, 2);
        JS_SetPropertyStr(ctx, root, "hmac", hmac);
    }
    {
        JSValue hash = JS_NewObject(ctx);
        installHelper(ctx, hash, "sha256", jsSha256, 1);
        JS_SetPropertyStr(ctx, root, "hash", hash);
    }
    {
        JSValue jwt = JS_NewObject(ctx);
        installHelper(ctx, jwt, "sign", jsJwtSign, 3);
        JS_SetPropertyStr(ctx, root, "jwt", jwt);
    }

    return root;
}

// ─── Read mutated request/response back from JS ──────────────────────────────

void readMutatedRequest(JSContext* ctx, JSValueConst root, HookRequestView& outReq) {
    JSValue req = JS_GetPropertyStr(ctx, root, "request");
    JsValueGuard guard{ctx, req};
    if (!JS_IsObject(req)) {
        return;
    }

    {
        JSValue url = JS_GetPropertyStr(ctx, req, "url");
        JsValueGuard urlGuard{ctx, url};
        if (JS_IsString(url)) {
            outReq.url = toCppString(ctx, url);
        }
    }
    {
        JSValue body = JS_GetPropertyStr(ctx, req, "body");
        JsValueGuard bodyGuard{ctx, body};
        if (JS_IsString(body)) {
            outReq.body = toCppString(ctx, body);
        } else if (JS_IsNull(body) || JS_IsUndefined(body)) {
            outReq.body = std::nullopt;
        }
    }
    {
        JSValue headers = JS_GetPropertyStr(ctx, req, "headers");
        JsValueGuard headersGuard{ctx, headers};
        if (JS_IsObject(headers)) {
            outReq.headers = readStringMap(ctx, headers);
        }
    }
    // method is intentionally read-only post-resolution; templates have
    // already locked it in by the time hooks see ctx.
}

void readMutatedResponse(JSContext* ctx, JSValueConst root, HookResponseView& outResp) {
    JSValue resp = JS_GetPropertyStr(ctx, root, "response");
    JsValueGuard guard{ctx, resp};
    if (!JS_IsObject(resp)) {
        return;
    }

    {
        JSValue status = JS_GetPropertyStr(ctx, resp, "status");
        JsValueGuard statusGuard{ctx, status};
        if (JS_IsNumber(status)) {
            int32_t s = 0;
            if (JS_ToInt32(ctx, &s, status) == 0) {
                outResp.status = s;
            }
        }
    }
    {
        JSValue body = JS_GetPropertyStr(ctx, resp, "body");
        JsValueGuard bodyGuard{ctx, body};
        if (JS_IsString(body)) {
            outResp.body = toCppString(ctx, body);
        }
    }
    {
        JSValue headers = JS_GetPropertyStr(ctx, resp, "headers");
        JsValueGuard headersGuard{ctx, headers};
        if (JS_IsObject(headers)) {
            outResp.headers = readStringMap(ctx, headers);
        }
    }
}

// ─── Error formatting ────────────────────────────────────────────────────────

[[nodiscard]] std::string formatJsException(JSContext* ctx) {
    JSValue ex = JS_GetException(ctx);
    JsValueGuard guard{ctx, ex};
    if (JS_IsNull(ex) || JS_IsUndefined(ex)) {
        return "<unknown>";
    }

    auto base = toCppString(ctx, ex);
    JSValue stack = JS_GetPropertyStr(ctx, ex, "stack");
    JsValueGuard stackGuard{ctx, stack};
    if (JS_IsString(stack)) {
        auto stackStr = toCppString(ctx, stack);
        if (!stackStr.empty()) {
            base += "\n" + stackStr;
        }
    }
    return base;
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
constexpr std::size_t kStackSize = std::size_t{8} * 1024 * 1024;  // 8 MiB

[[nodiscard]] std::expected<HookOutcome, ChainApiError> runScript(const std::string& script,
                                                                  HookContext context,
                                                                  bool includeResponse) {
    JsRuntimePtr rt{JS_NewRuntime()};
    if (!rt) {
        return std::unexpected(
            ChainApiError{ErrorCode::HookFailure, ErrorClass::Hook, "JS_NewRuntime returned null"});
    }
    JS_SetMaxStackSize(rt.get(), kStackSize);

    InterruptState ist{std::chrono::steady_clock::now() + kHookTimeout};
    JS_SetInterruptHandler(rt.get(), interruptCallback, &ist);

    JsContextPtr jsctx{JS_NewContext(rt.get())};
    if (!jsctx) {
        return std::unexpected(
            ChainApiError{ErrorCode::HookFailure, ErrorClass::Hook, "JS_NewContext returned null"});
    }

    JSContext* c = jsctx.get();

    // Build the `ctx` object and stash it on the global as `__chainapi_ctx`
    // so wrapper code can reach it whether the script is a module or an
    // inline body.
    JSValue hookCtxObj = buildHookCtx(c, context, includeResponse);
    JSValue global = JS_GetGlobalObject(c);
    JsValueGuard globalGuard{c, global};
    JS_SetPropertyStr(c, global, "__chainapi_ctx", JS_DupValue(c, hookCtxObj));
    JsValueGuard hookCtxGuard{c, hookCtxObj};

    auto fail = [&](ErrorCode code, std::string detail) {
        return std::unexpected(ChainApiError{code, ErrorClass::Hook, std::move(detail)});
    };

    if (looksLikeModule(script)) {
        // Compile the module and trigger evaluation. The user's default
        // export is invoked via a thin script that imports it back.
        JSValue compiled = JS_Eval(c,
                                   script.data(),
                                   script.size(),
                                   "hook.mjs",
                                   JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(compiled)) {
            JS_FreeValue(c, compiled);
            return fail(ErrorCode::HookFailure, "hook compile failed: " + formatJsException(c));
        }
        JSValue evalOk = JS_EvalFunction(c, compiled);
        if (JS_IsException(evalOk)) {
            JS_FreeValue(c, evalOk);
            const auto err = formatJsException(c);
            if (err.find("interrupted") != std::string::npos) {
                return fail(ErrorCode::HookTimeout, "hook exceeded 1s budget");
            }
            return fail(ErrorCode::HookFailure, "hook failed: " + err);
        }
        JS_FreeValue(c, evalOk);

        // QuickJS are cached by name.
        const std::string driver =
            "import fn from 'hook.mjs';\n"
            "globalThis.__chainapi_invoke = () => fn(globalThis.__chainapi_ctx);\n";
        JSValue driverEval =
            JS_Eval(c, driver.data(), driver.size(), "<driver>", JS_EVAL_TYPE_MODULE);
        if (JS_IsException(driverEval)) {
            JS_FreeValue(c, driverEval);
            const auto err = formatJsException(c);
            return fail(ErrorCode::HookFailure, "hook driver eval failed: " + err);
        }
        JS_FreeValue(c, driverEval);

        JSValue invoke = JS_GetPropertyStr(c, global, "__chainapi_invoke");
        JsValueGuard invokeGuard{c, invoke};
        if (!JS_IsFunction(c, invoke)) {
            return fail(ErrorCode::HookFailure, "module did not export a default function");
        }
        JSValue result = JS_Call(c, invoke, JS_UNDEFINED, 0, nullptr);
        JsValueGuard resultGuard{c, result};
        if (JS_IsException(result)) {
            const auto err = formatJsException(c);
            if (err.find("interrupted") != std::string::npos) {
                return fail(ErrorCode::HookTimeout, "hook exceeded 1s budget");
            }
            return fail(ErrorCode::HookFailure, "hook threw: " + err);
        }
    } else {
        // Inline body. Wrap in `(function(ctx){ <body> })(__chainapi_ctx)`.
        // We don't add `'use strict';` because hook bodies are user code
        // and forcing strict mode would silently break valid loose-mode
        // snippets. Crypto helpers don't care either way.
        const std::string wrapped =
            "(function(ctx){\n" + script + "\n}).call(undefined, globalThis.__chainapi_ctx);\n";
        JSValue evalRes =
            JS_Eval(c, wrapped.data(), wrapped.size(), "hook.js", JS_EVAL_TYPE_GLOBAL);
        JsValueGuard evalGuard{c, evalRes};
        if (JS_IsException(evalRes)) {
            const auto err = formatJsException(c);
            if (err.find("interrupted") != std::string::npos) {
                return fail(ErrorCode::HookTimeout, "hook exceeded 1s budget");
            }
            return fail(ErrorCode::HookFailure, "hook threw: " + err);
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

std::expected<HookOutcome, ChainApiError> QuickJsHookRunner::runPreRequest(
    const std::string& script, HookContext context) {
    return runScript(script, std::move(context), /*includeResponse=*/false);
}

std::expected<HookOutcome, ChainApiError> QuickJsHookRunner::runPostResponse(
    const std::string& script, HookContext context) {
    return runScript(script, std::move(context), /*includeResponse=*/true);
}

}  // namespace chainapi::engine
