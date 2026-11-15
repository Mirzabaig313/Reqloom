// ImportFromPostman — Postman Collection v2.1 (JSON) → Project parser.
// Engine importer, sibling of ImportFromOpenApi.

#include "ImportFromPostman.h"
#include "ImportFromFileRefs.h"
#include "ImportFromJsonHelpers.h"
#include "ImportSupport.h"

#include "../domain/DependencyResolver.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace reqloom::engine {

namespace {

namespace fs = std::filesystem;
namespace is = import_support;
using json = nlohmann::json;

/// Upper bound on the collection file we'll read (16 MiB); refuses unbounded files from an
/// untrusted path.
constexpr std::uintmax_t kMaxCollectionBytes = std::uintmax_t{16} * 1024 * 1024;

/// Bounds folder nesting so a malicious collection can't blow the stack.
constexpr int kMaxItemDepth = 64;

ReqloomError invalid(std::string detail) {
    return ReqloomError{ErrorCode::SchemaInvalid, ErrorClass::Schema, std::move(detail)};
}

// Containment: resolved path must live under projectRoot with a separator boundary (`/proj` must
// not admit `/proj-evil`).
[[nodiscard]] std::expected<fs::path, ReqloomError> canonicalPath(const fs::path& file,
                                                                  const fs::path& projectRoot) {
    std::error_code ec;
    auto canonical = fs::weakly_canonical(file, ec);
    if (ec) {
        return std::unexpected(
            invalid("postman import: cannot canonicalise collection path: " + ec.message()));
    }
    if (!fs::exists(canonical, ec) || !fs::is_regular_file(canonical, ec)) {
        return std::unexpected(
            invalid("postman import: collection is not a regular file: " + canonical.string()));
    }
    auto root = fs::weakly_canonical(projectRoot, ec);
    if (ec) {
        return std::unexpected(
            invalid("postman import: cannot canonicalise project root: " + ec.message()));
    }
    const auto canonStr = canonical.lexically_normal().string();
    const auto rootStr = root.lexically_normal().string();
    const bool contained = canonStr.size() >= rootStr.size() &&
                           canonStr.substr(0, rootStr.size()) == rootStr &&
                           (canonStr.size() == rootStr.size() || canonStr[rootStr.size()] == '/' ||
                            canonStr[rootStr.size()] == fs::path::preferred_separator);
    if (!contained) {
        return std::unexpected(invalid(
            "postman import: collection path resolves outside the project root (" + rootStr + ")"));
    }
    return canonical;
}

std::string nowIso8601Utc() {
    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string{buf};
}

std::string toLowerAscii(std::string_view s) {
    std::string out{s};
    for (auto& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + ('a' - 'A'));
        }
    }
    return out;
}

/// Reduce a Postman name to a reqloom-safe id: lowercase, non-alphanumeric runs become one '_',
/// ends trimmed; never contains '.', '/' or '\'. `fallback` when nothing remains.
std::string sanitizeId(std::string_view name, std::string_view fallback) {
    std::string out;
    out.reserve(name.size());
    bool pendingSep = false;
    for (const char c : name) {
        const bool alnum =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (alnum) {
            if (pendingSep && !out.empty()) {
                out.push_back('_');
            }
            pendingSep = false;
            out.push_back(static_cast<char>((c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c));
        } else {
            pendingSep = true;
        }
    }
    if (out.empty()) {
        return std::string{fallback};
    }
    return out;
}

HttpMethod parseMethod(std::string_view methodRaw) {
    const auto m = toLowerAscii(methodRaw);
    if (m == "post") {
        return HttpMethod::Post;
    }
    if (m == "put") {
        return HttpMethod::Put;
    }
    if (m == "patch") {
        return HttpMethod::Patch;
    }
    if (m == "delete") {
        return HttpMethod::Delete;
    }
    if (m == "head") {
        return HttpMethod::Head;
    }
    if (m == "options") {
        return HttpMethod::Options;
    }
    return HttpMethod::Get;
}

/// Rewrite Postman `{{var}}` to reqloom's `{{env.<key>}}` scope; a dotted name is one
/// variable, sanitised to an environment key. Dynamic variables (`{{$guid}}`) are untouched.
std::string rewriteTokens(std::string_view in,
                          is::VarRefs& referenced,
                          std::set<std::string>& dynamic) {
    std::string out;
    out.reserve(in.size());
    std::size_t i = 0;
    while (i < in.size()) {
        const auto open = in.find("{{", i);
        if (open == std::string_view::npos) {
            out.append(in.substr(i));
            break;
        }
        out.append(in.substr(i, open - i));
        const auto close = in.find("}}", open + 2);
        if (close == std::string_view::npos) {
            out.append(in.substr(open));
            break;
        }
        std::string_view inner = in.substr(open + 2, close - (open + 2));
        while (!inner.empty() && (inner.front() == ' ' || inner.front() == '\t')) {
            inner.remove_prefix(1);
        }
        while (!inner.empty() && (inner.back() == ' ' || inner.back() == '\t')) {
            inner.remove_suffix(1);
        }
        if (inner.empty()) {
            out.append("{{}}");
        } else if (inner.front() == '$') {
            dynamic.emplace(inner);
            out.append("{{").append(inner).append("}}");
        } else {
            out.append(is::envRef(inner, referenced));
        }
        i = close + 2;
    }
    return out;
}

/// A Postman URL split into base (scheme+authority or a resolved base variable) and path; query is
/// handled separately.
struct SplitUrl {
    std::string base;  ///< e.g. "https://api.example.com" — may be empty.
    std::string path;  ///< always begins with '/'.
};

/// Split a raw URL into base + path, resolving a leading `{{var}}` from the collection variables
/// when possible.
SplitUrl splitRawUrl(std::string_view raw, const std::map<std::string, std::string>& vars) {
    auto end = raw.find_first_of("?#");
    std::string_view noQuery = raw.substr(0, end == std::string_view::npos ? raw.size() : end);

    SplitUrl out;
    if (noQuery.starts_with("{{")) {
        const auto close = noQuery.find("}}");
        if (close != std::string_view::npos) {
            std::string_view name = noQuery.substr(2, close - 2);
            while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) {
                name.remove_prefix(1);
            }
            while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
                name.remove_suffix(1);
            }
            if (const auto it = vars.find(is::envKey(name)); it != vars.end()) {
                out.base = it->second;
            }
            std::string_view rest = noQuery.substr(close + 2);
            out.path = rest.empty() ? "/" : std::string{rest};
        }
    } else if (const auto scheme = noQuery.find("://"); scheme != std::string_view::npos) {
        const auto authStart = scheme + 3;
        const auto slash = noQuery.find('/', authStart);
        if (slash == std::string_view::npos) {
            out.base = std::string{noQuery};
            out.path = "/";
        } else {
            out.base = std::string{noQuery.substr(0, slash)};
            out.path = std::string{noQuery.substr(slash)};
        }
    } else {
        out.path = std::string{noQuery};
    }

    if (out.path.empty()) {
        out.path = "/";
    } else if (out.path.front() != '/') {
        out.path.insert(out.path.begin(), '/');
    }
    return out;
}

std::string jsonStr(const json& node, std::string_view key) {
    if (node.is_object()) {
        if (const auto it = node.find(key); it != node.end() && it->is_string()) {
            return it->get<std::string>();
        }
    }
    return {};
}

/// Source path(s) of a Postman `formdata` file field: `src` is a string or an array; empty or
/// absent yields none.
std::vector<std::string> postmanFileSrcs(const json& field) {
    std::vector<std::string> out;
    const auto it = field.find("src");
    if (it == field.end()) {
        return out;
    }
    if (it->is_string() && !it->get<std::string>().empty()) {
        out.push_back(it->get<std::string>());
    } else if (it->is_array()) {
        for (const auto& s : *it) {
            if (s.is_string() && !s.get<std::string>().empty()) {
                out.push_back(s.get<std::string>());
            }
        }
    }
    return out;
}

}  // namespace

std::expected<ImportFromPostman::Outcome, ReqloomError> ImportFromPostman::run(
    const fs::path& collection, const fs::path& projectRoot) const {
    const auto pathOr = canonicalPath(collection, projectRoot);
    if (!pathOr) {
        return std::unexpected(pathOr.error());
    }

    std::error_code ec;
    const auto size = fs::file_size(*pathOr, ec);
    if (!ec && size > kMaxCollectionBytes) {
        return std::unexpected(
            invalid(std::format("postman import: collection is too large ({} bytes; limit {})",
                                size,
                                kMaxCollectionBytes)));
    }

    std::ifstream in(*pathOr, std::ios::binary);
    if (!in) {
        return std::unexpected(invalid("postman import: cannot open collection file"));
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();

    json doc;
    try {
        doc = json::parse(text);
    } catch (const json::parse_error& e) {
        return std::unexpected(
            ReqloomError{ErrorCode::YamlParse,
                         ErrorClass::Schema,
                         std::string{"postman import: invalid JSON: "} + e.what()});
    }

    if (!doc.is_object() || !doc.contains("info") || !doc.contains("item") ||
        !doc["item"].is_array()) {
        return std::unexpected(
            invalid("postman import: not a Postman collection (missing info/item)"));
    }
    // Accept v2.0 and v2.1; reject other shapes.
    const auto schema = jsonStr(doc["info"], "schema");
    if (!schema.empty() && schema.find("v2.1") == std::string::npos &&
        schema.find("v2.0") == std::string::npos) {
        return std::unexpected(invalid(std::format(
            "postman import: unsupported collection schema ({}); export as Collection v2.1",
            schema)));
    }

    Outcome outcome;
    std::vector<std::string> warnings;

    const std::string collectionName = jsonStr(doc["info"], "name");
    outcome.project.name = collectionName.empty() ? "Imported Collection" : collectionName;
    outcome.project.defaultEnvironment = "default";
    auto& env = outcome.project.environments["default"];

    if (doc.contains("variable") && doc["variable"].is_array()) {
        for (const auto& v : doc["variable"]) {
            const auto key = jsonStr(v, "key");
            if (!key.empty()) {
                env[is::envKey(key)] = jsonStr(v, "value");
                if (jsonStr(v, "type") == "secret") {
                    outcome.secretVariables.push_back({"default", is::envKey(key)});
                }
            }
        }
    }

    const auto importedAt = nowIso8601Utc();
    const std::string defaultResource = sanitizeId(collectionName, "requests");

    is::VarRefs referencedVars;
    std::set<std::string> dynamicVars;
    std::set<std::string> seenOpIds;
    std::string projectBase;  // first non-empty base wins (see ceiling below)
    bool multipleBasesWarned = false;

    // Top-level folders define resources; deeper folders fold into the ancestor resource;
    // root-level requests land in a resource named after the collection.
    auto rewrite = [&](std::string_view s) {
        return rewriteTokens(s, referencedVars, dynamicVars);
    };

    auto buildRequest = [&](const std::string& resourceId, const json& item) {
        auto& resource = outcome.project.resources[ResourceId{resourceId}];
        if (resource.id.value.empty()) {
            resource.id = ResourceId{resourceId};
        }

        const std::string reqName = jsonStr(item, "name");
        std::string opName = sanitizeId(reqName, "request");
        std::string unique = opName;
        int suffix = 2;
        while (resource.operations.contains(unique) ||
               seenOpIds.contains(std::format("{}.{}", resourceId, unique))) {
            unique = std::format("{}_{}", opName, suffix++);
        }
        seenOpIds.insert(std::format("{}.{}", resourceId, unique));

        const json& request = item["request"];

        Operation op;
        op.id = OperationId{std::format("{}.{}", resourceId, unique)};
        op.resource = ResourceId{resourceId};
        op.method = parseMethod(request.is_object() ? jsonStr(request, "method") : "get");

        std::string rawUrl;
        const json* urlNode = nullptr;
        if (request.is_object() && request.contains("url")) {
            const json& u = request["url"];
            if (u.is_string()) {
                rawUrl = u.get<std::string>();
            } else if (u.is_object()) {
                urlNode = &u;
                rawUrl = jsonStr(u, "raw");
                if (rawUrl.empty() && u.contains("path") && u["path"].is_array()) {
                    std::string joined;
                    for (const auto& seg : u["path"]) {
                        if (seg.is_string()) {
                            joined.push_back('/');
                            joined.append(seg.get<std::string>());
                        }
                    }
                    rawUrl = joined;
                }
            }
        }
        const auto split = splitRawUrl(rawUrl, env);
        if (!split.base.empty()) {
            if (projectBase.empty()) {
                projectBase = split.base;
            } else if (projectBase != split.base && !multipleBasesWarned) {
                warnings.push_back(std::format(
                    "requests target more than one host; the project baseUrl was set to `{}` — "
                    "review operations that use a different host",
                    projectBase));
                multipleBasesWarned = true;
            }
        }
        const auto pathParams = is::convertPathParams(rewrite(split.path), resourceId);
        op.pathTemplate = pathParams.path;
        is::VarMap pathValues;
        if (urlNode != nullptr && urlNode->contains("variable") &&
            (*urlNode)["variable"].is_array()) {
            for (const auto& v : (*urlNode)["variable"]) {
                if (const auto key = jsonStr(v, "key"); !key.empty()) {
                    pathValues[is::identifier(key, "param")] = jsonStr(v, "value");
                }
            }
        }

        if (urlNode != nullptr && urlNode->contains("query") && (*urlNode)["query"].is_array()) {
            for (const auto& q : (*urlNode)["query"]) {
                if (!q.is_object() || import_json::boolOr(q, "disabled", false)) {
                    continue;
                }
                const auto key = jsonStr(q, "key");
                if (!key.empty()) {
                    op.queryParams[key] = rewrite(jsonStr(q, "value"));
                }
            }
        }

        if (request.is_object() && request.contains("header") && request["header"].is_array()) {
            for (const auto& h : request["header"]) {
                if (!h.is_object() || import_json::boolOr(h, "disabled", false)) {
                    continue;
                }
                const auto key = jsonStr(h, "key");
                if (!key.empty()) {
                    op.headers[key] = rewrite(jsonStr(h, "value"));
                }
            }
        }

        if (request.is_object() && request.contains("body") && request["body"].is_object()) {
            const json& body = request["body"];
            const auto mode = jsonStr(body, "mode");
            if (mode == "raw") {
                op.bodyTemplate = rewrite(jsonStr(body, "raw"));
            } else if (mode == "urlencoded" && body.contains("urlencoded") &&
                       body["urlencoded"].is_array()) {
                std::map<std::string, std::string> form;
                for (const auto& f : body["urlencoded"]) {
                    if (!f.is_object() || import_json::boolOr(f, "disabled", false)) {
                        continue;
                    }
                    const auto key = jsonStr(f, "key");
                    if (!key.empty()) {
                        form[key] = import_files::textFieldValue(
                            jsonStr(f, "value"), op.id.value, key, warnings, rewrite);
                    }
                }
                if (!form.empty()) {
                    op.bodyForm = std::move(form);
                }
            } else if (mode == "formdata" && body.contains("formdata") &&
                       body["formdata"].is_array()) {
                std::map<std::string, std::string> form;
                for (const auto& f : body["formdata"]) {
                    if (!f.is_object() || import_json::boolOr(f, "disabled", false)) {
                        continue;
                    }
                    const auto key = jsonStr(f, "key");
                    if (key.empty()) {
                        continue;
                    }
                    if (jsonStr(f, "type") == "file") {
                        // Map to the `@path` multipart convention; Postman exports file fields with
                        // no path, so a bare `@` placeholder is normal and needs no warning.
                        const auto srcs = postmanFileSrcs(f);
                        if (srcs.empty()) {
                            form[key] = "@";
                        } else {
                            form[key] = import_files::fileFieldValue(
                                srcs.front(), op.id.value, key, warnings, rewrite);
                            if (srcs.size() > 1) {
                                warnings.push_back(std::format(
                                    "operation {}: file field `{}` had {} files; only the first "
                                    "was imported",
                                    op.id.value,
                                    key,
                                    srcs.size()));
                            }
                        }
                    } else {
                        form[key] = import_files::textFieldValue(
                            jsonStr(f, "value"), op.id.value, key, warnings, rewrite);
                    }
                }
                if (!form.empty()) {
                    op.bodyForm = std::move(form);
                    is::keepMultipart(op);
                }
            } else if (mode == "graphql" && body.contains("graphql") &&
                       body["graphql"].is_object()) {
                json gql;
                gql["query"] = jsonStr(body["graphql"], "query");
                const auto varsRaw = jsonStr(body["graphql"], "variables");
                if (!varsRaw.empty()) {
                    gql["variables"] = varsRaw;
                }
                op.bodyTemplate = rewrite(gql.dump());
            }
        }

        Provenance prov;
        prov.source = Provenance::Source::PostmanImport;
        prov.verifiedAgainst = Provenance::VerifiedAgainst::None;
        prov.importedAt = importedAt;
        if (!reqName.empty()) {
            prov.evidence["postman_name"] = reqName;
        }
        is::notePathParams(pathParams, op.id.value, resourceId, pathValues, prov, warnings);
        if (request.is_object() && request.contains("auth") &&
            is::warnDroppedAuth(
                warnings, "operation " + op.id.value, jsonStr(request["auth"], "type"))) {
            prov.evidence["auth"] = "request declared Postman auth — configure an actor to wire it";
        }
        op.provenance = std::move(prov);

        resource.operations.emplace(unique, std::move(op));
    };

    auto walk =
        [&](const json& items, const std::string& resourceId, int depth, auto&& self) -> void {
        if (depth > kMaxItemDepth) {
            warnings.push_back(
                "folder nesting exceeded the supported depth; deeper items were "
                "skipped");
            return;
        }
        for (const auto& item : items) {
            if (!item.is_object()) {
                continue;
            }
            if (item.contains("item") && item["item"].is_array()) {
                if (item.contains("auth")) {
                    is::warnDroppedAuth(warnings,
                                        std::format("folder `{}`", jsonStr(item, "name")),
                                        jsonStr(item["auth"], "type"));
                }
                const std::string childResource =
                    resourceId.empty() ? sanitizeId(jsonStr(item, "name"), defaultResource)
                                       : resourceId;
                self(item["item"], childResource, depth + 1, self);
            } else if (item.contains("request")) {
                buildRequest(resourceId.empty() ? defaultResource : resourceId, item);
            }
        }
    };
    if (doc.contains("auth")) {
        is::warnDroppedAuth(warnings,
                            std::format("collection `{}`", outcome.project.name),
                            jsonStr(doc["auth"], "type"));
    }
    walk(doc["item"], std::string{}, 0, walk);

    if (outcome.project.resources.empty()) {
        return std::unexpected(
            invalid("postman import: collection yielded zero importable requests"));
    }

    // A single project-level baseUrl from the first request's host; collections hitting multiple
    // hosts get a warning (emitted above).
    if (!projectBase.empty()) {
        env["baseUrl"] = projectBase;
    } else if (!env.contains("baseUrl")) {
        warnings.push_back(
            "no host could be derived from the requests — set `baseUrl` in the environment");
    }

    // Surface variables that requests reference but the collection never
    // defined, so they load and can be filled in the environment editor.
    is::warnRenamedVars(referencedVars, warnings);
    for (const auto& ref : referencedVars.referenced) {
        if (!env.contains(ref)) {
            env[ref] = "";
            warnings.push_back(std::format(
                "variable `{{{{{}}}}}` is referenced but was not defined — set it in the "
                "environment",
                ref));
        }
    }
    for (const auto& dyn : dynamicVars) {
        warnings.push_back(
            std::format("dynamic variable `{{{{{}}}}}` is a Postman runtime value with no reqloom "
                        "equivalent — replace it",
                        dyn));
    }

    // Same structural validation the YAML parser and OpenAPI importer run.
    if (auto valid = DependencyResolver{}.validate(outcome.project); !valid) {
        return std::unexpected(valid.error());
    }

    std::ostringstream wbuf;
    for (const auto& w : warnings) {
        wbuf << w << '\n';
    }
    outcome.warnings = wbuf.str();
    return outcome;
}

}  // namespace reqloom::engine
