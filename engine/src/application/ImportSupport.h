// Shared logic for the collection importers: identifier and environment-key
// sanitising, variable-reference mapping, path-parameter conversion, auth
// detection and environment merging. Each importer keeps its own parsing;
// these helpers make the output the same whatever the source format.
#pragma once

#include <reqloom/engine/Operation.h>

#include <cstdint>
#include <format>
#include <map>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace reqloom::engine::import_support {

using VarMap = std::map<std::string, std::string>;

/// Characters kept as-is in an imported identifier. Anything else (`.`, `$`,
/// spaces, `/`, ...) would break `{{scope.field}}` splitting or a file name.
[[nodiscard]] constexpr bool isIdChar(const char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '-';
}

/// `name` with every run of characters outside [A-Za-z0-9_-] replaced by one
/// `_`, and underscores that replacement put at either end trimmed. Case is
/// kept, so a name that is already valid comes back unchanged. `fallback`
/// when nothing is left.
[[nodiscard]] inline std::string identifier(std::string_view name, std::string_view fallback) {
    std::string out;
    out.reserve(name.size());
    bool pendingSep{false};
    for (const char c : name) {
        if (!isIdChar(c)) {
            pendingSep = true;
            continue;
        }
        if (pendingSep && !out.empty()) {
            out.push_back('_');
        }
        pendingSep = false;
        out.push_back(c);
    }
    return out.empty() ? std::string{fallback} : out;
}

/// Environment key for a source variable name (`api.url` → `api_url`). Use it
/// both where a value is imported and where it is referenced so they match.
[[nodiscard]] inline std::string envKey(std::string_view sourceName) {
    return identifier(sourceName, "var");
}

/// Variable references collected while rewriting an import.
struct VarRefs {
    std::set<std::string> referenced;  ///< environment keys referenced
    VarMap renamed;                    ///< source name → environment key, where they differ
};

/// `{{env.<key>}}` for a source variable reference. Source formats have no
/// Reqloom scopes, so a dotted name (`api.key`, Insomnia `_.api.url`, REST
/// Client `login.response.body.$.token`) is one variable name, not a
/// `scope.field` reference; it is sanitised rather than passed through.
[[nodiscard]] inline std::string envRef(std::string_view sourceName, VarRefs& refs) {
    auto key = envKey(sourceName);
    if (key != sourceName) {
        refs.renamed.emplace(std::string{sourceName}, key);
    }
    refs.referenced.insert(key);
    return std::format("{{{{env.{}}}}}", key);
}

/// One warning per source variable whose name had to change.
inline void warnRenamedVars(const VarRefs& refs,
                            std::vector<std::string>& warnings,
                            std::string_view open = "{{",
                            std::string_view close = "}}") {
    for (const auto& [source, key] : refs.renamed) {
        warnings.push_back(std::format(
            "variable `{}{}{}` is not a valid Reqloom variable name; it was imported as "
            "`{{{{env.{}}}}}` — check the environment value `{}`",
            open,
            source,
            close,
            key,
            key));
    }
}

/// Add `key = ""` to every environment that lacks a referenced key. Returns
/// true when at least one environment was missing it.
inline bool seedMissing(std::map<std::string, VarMap>& envs, const std::string& key) {
    bool missing{false};
    for (auto& [_, vars] : envs) {
        if (!vars.contains(key)) {
            vars[key] = "";
            missing = true;
        }
    }
    return missing;
}

/// Set `baseUrl` in every environment. When the base came from a leading
/// `{{var}}`, an environment that defines that variable gets its own value;
/// the others get `projectBase`.
inline void applyBaseUrl(std::map<std::string, VarMap>& envs,
                         const std::string& projectBase,
                         const std::string& baseVarKey) {
    for (auto& [_, vars] : envs) {
        const auto it = baseVarKey.empty() ? vars.end() : vars.find(baseVarKey);
        vars["baseUrl"] = it != vars.end() && !it->second.empty() ? it->second : projectBase;
    }
}

// ── Path parameters ──────────────────────────────────────────────────────────

/// A path with its parameters converted to Reqloom references.
struct PathParams {
    std::string path;
    std::vector<std::string> names;  ///< sanitised parameter names, in order
};

/// Convert path parameters to `{{<resourceId>.<name>}}`, the form the
/// OpenAPI importer has always produced: the value comes from an upstream
/// operation of the same resource that extracts `<name>`. Accepts both
/// `{name}` (OpenAPI, Apidog, Thunder Client) and a `:name` segment start
/// (Postman, Bruno). Existing `{{...}}` references are left alone, so call
/// this after the importer has rewritten its variable references.
[[nodiscard]] inline PathParams convertPathParams(std::string_view path,
                                                  std::string_view resourceId) {
    PathParams out;
    out.path.reserve(path.size());
    const auto emit = [&](std::string_view raw) {
        auto name = identifier(raw, "param");
        out.path.append(std::format("{{{{{}.{}}}}}", resourceId, name));
        bool seen{false};
        for (const auto& n : out.names) {
            seen = seen || n == name;
        }
        if (!seen) {
            out.names.push_back(std::move(name));
        }
    };
    std::size_t i{0};
    while (i < path.size()) {
        if (path.substr(i).starts_with("{{")) {
            const auto close = path.find("}}", i + 2);
            const auto end = close == std::string_view::npos ? path.size() : close + 2;
            out.path.append(path.substr(i, end - i));
            i = end;
            continue;
        }
        const char c = path[i];
        if (c == '{') {
            const auto close = path.find('}', i + 1);
            const auto name = close == std::string_view::npos ? std::string_view{}
                                                              : path.substr(i + 1, close - i - 1);
            if (!name.empty() && name.find('{') == std::string_view::npos) {
                emit(name);
                i = close + 1;
                continue;
            }
        } else if (c == ':' && (i == 0 || path[i - 1] == '/')) {
            std::size_t j{i + 1};
            while (j < path.size() && isIdChar(path[j])) {
                ++j;
            }
            if (j > i + 1) {
                emit(path.substr(i + 1, j - i - 1));
                i = j;
                continue;
            }
        }
        out.path.push_back(c);
        ++i;
    }
    return out;
}

/// Record each converted path parameter: one evidence entry and one warning
/// per parameter, naming the variable an upstream operation must extract.
/// `sourceValues` holds any value the source format gave the parameter.
inline void notePathParams(const PathParams& params,
                           std::string_view opId,
                           std::string_view resourceId,
                           const VarMap& sourceValues,
                           Provenance& prov,
                           std::vector<std::string>& warnings) {
    for (const auto& param : params.names) {
        prov.evidence["path_param." + param] =
            std::format("rewritten to {{{{{}.{}}}}}; ensure an upstream operation extracts {}",
                        resourceId,
                        param,
                        param);
        std::string sourceValue;
        if (const auto it = sourceValues.find(param);
            it != sourceValues.end() && !it->second.empty()) {
            sourceValue = std::format(" (the source gave it the value `{}`)", it->second);
        }
        warnings.push_back(std::format(
            "operation {}: path parameter `{}` resolves to {{{{{}.{}}}}} — wire an upstream "
            "operation that extracts `{}` into resource `{}`{}",
            opId,
            param,
            resourceId,
            param,
            param,
            resourceId,
            sourceValue));
    }
}

// ── Auth ─────────────────────────────────────────────────────────────────────

/// What an importer found in a source auth setting.
enum class AuthKind : std::uint8_t {
    None,     ///< no auth (absent, empty, `none`, `noauth`, disabled)
    Inherit,  ///< inherits from a parent; the parent's own setting is reported there
    Dropped,  ///< a real auth scheme the importer does not convert
};

/// Classify a source auth type name. Case-insensitive.
[[nodiscard]] inline AuthKind classifyAuthType(std::string_view type) {
    std::string t{type};
    for (auto& c : t) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + ('a' - 'A'));
        }
    }
    if (t.empty() || t == "none" || t == "noauth" || t == "no-auth" || t == "no_auth") {
        return AuthKind::None;
    }
    if (t == "inherit" || t == "inherited" || t == "inherit-from-parent" || t == "parent") {
        return AuthKind::Inherit;
    }
    return AuthKind::Dropped;
}

/// The one warning for auth that was not imported. `where` is
/// "operation <id>", "collection `<name>`" or "folder `<name>`". Returns
/// true when a warning was added (only for AuthKind::Dropped).
inline bool warnDroppedAuth(std::vector<std::string>& warnings,
                            std::string_view where,
                            std::string_view type) {
    if (classifyAuthType(type) != AuthKind::Dropped) {
        return false;
    }
    warnings.push_back(std::format(
        "{}: `{}` auth was not imported — create an actor for it and attach it", where, type));
    return true;
}

// ── Bodies ───────────────────────────────────────────────────────────────────

/// Keep a source multipart form multipart: the engine sends a form with no file field
/// urlencoded unless the operation has `Content-Type: multipart/form-data`. An explicit
/// source Content-Type is left alone.
inline void keepMultipart(Operation& op) {
    if (!op.bodyForm) {
        return;
    }
    for (const auto& name : op.headers | std::views::keys) {
        std::string lower{name};
        for (auto& c : lower) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c + ('a' - 'A'));
            }
        }
        if (lower == "content-type") {
            return;
        }
    }
    op.headers["Content-Type"] = "multipart/form-data";
}

// ── Environments ─────────────────────────────────────────────────────────────

/// Environment name for a source environment: lowercase, file-safe, unique
/// among `taken`.
[[nodiscard]] inline std::string environmentName(std::string_view sourceName,
                                                 const std::map<std::string, VarMap>& taken) {
    std::string base;
    bool pendingSep{false};
    for (const char c : sourceName) {
        const bool alnum =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!alnum) {
            pendingSep = true;
            continue;
        }
        if (pendingSep && !base.empty()) {
            base.push_back('_');
        }
        pendingSep = false;
        base.push_back(static_cast<char>((c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c));
    }
    if (base.empty()) {
        base = "environment";
    }
    std::string name = base;
    for (int n{2}; taken.contains(name); ++n) {
        name = std::format("{}_{}", base, n);
    }
    return name;
}

/// Copy `source` into `target`. Keys `target` already held with a different
/// value are overwritten and appended to `overridden`.
inline void mergeVars(VarMap& target, const VarMap& source, std::vector<std::string>& overridden) {
    for (const auto& [key, value] : source) {
        const auto it = target.find(key);
        if (it != target.end() && it->second != value) {
            overridden.push_back(key);
        }
        target[key] = value;
    }
}

/// `a`, `b`, `c` for a warning message.
[[nodiscard]] inline std::string joinKeys(const std::vector<std::string>& keys) {
    std::string out;
    for (const auto& k : keys) {
        out += (out.empty() ? "`" : ", `") + k + "`";
    }
    return out;
}

}  // namespace reqloom::engine::import_support
