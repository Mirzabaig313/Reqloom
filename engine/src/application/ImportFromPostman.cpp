// ImportFromPostman — Postman Collection v2.1 (JSON) → Project parser.
// Engine importer, sibling of ImportFromOpenApi.

#include "ImportFromPostman.h"

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
using json = nlohmann::json;

/// Upper bound on the collection file we'll read (16 MiB). A Postman export is
/// text; anything larger is almost certainly not a hand-authored collection and
/// we refuse it rather than buffer an unbounded file from an untrusted path.
constexpr std::uintmax_t kMaxCollectionBytes = std::uintmax_t{16} * 1024 * 1024;

/// Bounds folder nesting so a pathological (or malicious) collection can't blow
/// the stack. Postman UIs rarely nest beyond a handful of levels.
constexpr int kMaxItemDepth = 64;

ReqloomError invalid(std::string detail) {
    return ReqloomError{ErrorCode::SchemaInvalid, ErrorClass::Schema, std::move(detail)};
}

// Containment check mirrors ImportFromOpenApi: the resolved path must live under
// projectRoot with a separator boundary so `/proj` doesn't admit `/proj-evil`.
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

/// Reduce an arbitrary Postman name to a reqloom-safe id: lowercase, every run
/// of non-alphanumeric characters becomes a single '_', with leading/trailing
/// underscores trimmed. Never contains '.', '/', or '\'. Falls back to
/// `fallback` when the input reduces to nothing.
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

/// Rewrite bare Postman `{{var}}` references to reqloom's `{{env.var}}` scope so
/// they resolve against the imported environment. References that are already
/// scoped (contain a '.') or are Postman dynamic variables (`{{$guid}}`) are
/// left untouched. Every rewritten variable name is recorded in `referenced`.
std::string rewriteTokens(std::string_view in,
                          std::set<std::string>& referenced,
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
        // Trim ASCII whitespace around the name.
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
        } else if (inner.find('.') != std::string_view::npos) {
            out.append("{{").append(inner).append("}}");
        } else {
            referenced.emplace(inner);
            out.append("{{env.").append(inner).append("}}");
        }
        i = close + 2;
    }
    return out;
}

/// A Postman URL split into a base (scheme+authority, or a resolved base
/// variable) and the path portion. Query handling is done separately from the
/// url object's `query` array when present.
struct SplitUrl {
    std::string base;  ///< e.g. "https://api.example.com" — may be empty.
    std::string path;  ///< always begins with '/'.
};

/// Split a raw URL string into base + path, resolving a leading `{{var}}` from
/// the collection variables when possible.
SplitUrl splitRawUrl(std::string_view raw, const std::map<std::string, std::string>& vars) {
    // Drop query + fragment; the path is everything before them.
    auto end = raw.find_first_of("?#");
    std::string_view noQuery = raw.substr(0, end == std::string_view::npos ? raw.size() : end);

    SplitUrl out;
    if (noQuery.starts_with("{{")) {
        const auto close = noQuery.find("}}");
        if (close != std::string_view::npos) {
            const std::string varName{noQuery.substr(2, close - 2)};
            if (const auto it = vars.find(varName); it != vars.end()) {
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

/// Collect the source path(s) of a Postman `formdata` file field. `src` may be
/// a single string or an array of strings (multiple files under one key); empty
/// entries and a null/absent `src` yield none.
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
    // v2.1 schema URL check — accept v2.0 and v2.1, reject clearly-other shapes.
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

    // Collection-level variables seed the environment. These are the values
    // {{env.var}} references resolve against.
    if (doc.contains("variable") && doc["variable"].is_array()) {
        for (const auto& v : doc["variable"]) {
            const auto key = jsonStr(v, "key");
            if (!key.empty()) {
                env[key] = jsonStr(v, "value");
            }
        }
    }

    const auto importedAt = nowIso8601Utc();
    const std::string defaultResource = sanitizeId(collectionName, "requests");

    std::set<std::string> referencedVars;
    std::set<std::string> dynamicVars;
    std::set<std::string> seenOpIds;
    std::string projectBase;  // first non-empty base wins (see ceiling below)
    bool multipleBasesWarned = false;

    // Recursive walk over the item tree. Top-level folders define resources;
    // deeper folders fold their requests into the ancestor resource (op-name
    // disambiguation keeps ids unique). Root-level requests land in a resource
    // named after the collection.
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

        // ── URL → base + path (+ query) ─────────────────────────────────────
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
        op.pathTemplate = rewrite(split.path);

        // Query params: prefer the url object's structured array.
        if (urlNode != nullptr && urlNode->contains("query") && (*urlNode)["query"].is_array()) {
            for (const auto& q : (*urlNode)["query"]) {
                if (q.value("disabled", false)) {
                    continue;
                }
                const auto key = jsonStr(q, "key");
                if (!key.empty()) {
                    op.queryParams[key] = rewrite(jsonStr(q, "value"));
                }
            }
        }

        // Headers.
        if (request.is_object() && request.contains("header") && request["header"].is_array()) {
            for (const auto& h : request["header"]) {
                if (h.value("disabled", false)) {
                    continue;
                }
                const auto key = jsonStr(h, "key");
                if (!key.empty()) {
                    op.headers[key] = rewrite(jsonStr(h, "value"));
                }
            }
        }

        // Body.
        if (request.is_object() && request.contains("body") && request["body"].is_object()) {
            const json& body = request["body"];
            const auto mode = jsonStr(body, "mode");
            if (mode == "raw") {
                op.bodyTemplate = rewrite(jsonStr(body, "raw"));
            } else if (mode == "urlencoded" && body.contains("urlencoded") &&
                       body["urlencoded"].is_array()) {
                std::map<std::string, std::string> form;
                for (const auto& f : body["urlencoded"]) {
                    if (f.value("disabled", false)) {
                        continue;
                    }
                    const auto key = jsonStr(f, "key");
                    if (!key.empty()) {
                        form[key] = rewrite(jsonStr(f, "value"));
                    }
                }
                if (!form.empty()) {
                    op.bodyForm = std::move(form);
                }
            } else if (mode == "formdata" && body.contains("formdata") &&
                       body["formdata"].is_array()) {
                std::map<std::string, std::string> form;
                for (const auto& f : body["formdata"]) {
                    if (f.value("disabled", false)) {
                        continue;
                    }
                    const auto key = jsonStr(f, "key");
                    if (key.empty()) {
                        continue;
                    }
                    if (jsonStr(f, "type") == "file") {
                        // Map to reqloom's `@path` file-field convention so the
                        // request is sent as multipart/form-data with the file
                        // attached (MultipartBuilder resolves the leading `@`).
                        // Postman exports file fields with no path, so a bare
                        // `@` placeholder is the normal case — the editor shows
                        // the row as "Choose a file…", so no warning is needed.
                        const auto srcs = postmanFileSrcs(f);
                        if (srcs.empty()) {
                            form[key] = "@";
                        } else {
                            form[key] = "@" + rewrite(srcs.front());
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
                        form[key] = rewrite(jsonStr(f, "value"));
                    }
                }
                if (!form.empty()) {
                    op.bodyForm = std::move(form);
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
        if (request.is_object() && request.contains("auth")) {
            prov.evidence["auth"] = "request declared Postman auth — configure an actor to wire it";
            warnings.push_back(std::format(
                "operation {}: request-level auth was not imported — create an actor and attach "
                "it",
                op.id.value));
        }
        op.provenance = std::move(prov);

        resource.operations.emplace(unique, std::move(op));
    };

    // Iterative-safe recursion via an explicit lambda with depth guard.
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
                // Folder. A top-level folder (resourceId empty) becomes its own
                // resource; nested folders fold into the ancestor resource.
                const std::string childResource =
                    resourceId.empty() ? sanitizeId(jsonStr(item, "name"), defaultResource)
                                       : resourceId;
                self(item["item"], childResource, depth + 1, self);
            } else if (item.contains("request")) {
                buildRequest(resourceId.empty() ? defaultResource : resourceId, item);
            }
        }
    };
    walk(doc["item"], std::string{}, 0, walk);

    if (outcome.project.resources.empty()) {
        return std::unexpected(
            invalid("postman import: collection yielded zero importable requests"));
    }

    // Set the environment baseUrl from the first request's host. A single
    // project-level baseUrl is a known simplification — collections that hit
    // multiple hosts get one baseUrl plus a warning (emitted above).
    // ponytail: single baseUrl per project; multi-host would need per-op URLs.
    if (!projectBase.empty()) {
        env["baseUrl"] = projectBase;
    } else if (!env.contains("baseUrl")) {
        warnings.push_back(
            "no host could be derived from the requests — set `baseUrl` in the environment");
    }

    // Surface variables that requests reference but the collection never
    // defined, so they load and can be filled in the environment editor.
    for (const auto& ref : referencedVars) {
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
