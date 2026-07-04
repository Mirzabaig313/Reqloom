// ImportFromInsomnia — Insomnia v4 (JSON) → Project parser.
// Sibling of ImportFromPostman. Insomnia exports a flat `resources` array whose
// nodes (`workspace`/`request_group`/`request`/`environment`) reference their
// parent by `parentId`; this walks that graph into reqloom resources/operations.
//
// the path/id/url helpers mirror ImportFromPostman rather than share a
// common header — keeps this parser self-contained (the existing importers do
// the same). Upgrade path: extract a shared ImportSupport.h once a third parser
// needs the identical helpers.

#include "ImportFromInsomnia.h"

#include "../domain/DependencyResolver.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace reqloom::engine {

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

constexpr std::uintmax_t kMaxExportBytes = std::uintmax_t{16} * 1024 * 1024;

ReqloomError invalid(std::string detail) {
    return ReqloomError{ErrorCode::SchemaInvalid, ErrorClass::Schema, std::move(detail)};
}

[[nodiscard]] std::expected<fs::path, ReqloomError> canonicalPath(const fs::path& file,
                                                                  const fs::path& projectRoot) {
    std::error_code ec;
    auto canonical = fs::weakly_canonical(file, ec);
    if (ec) {
        return std::unexpected(
            invalid("insomnia import: cannot canonicalise export path: " + ec.message()));
    }
    if (!fs::exists(canonical, ec) || !fs::is_regular_file(canonical, ec)) {
        return std::unexpected(
            invalid("insomnia import: export is not a regular file: " + canonical.string()));
    }
    auto root = fs::weakly_canonical(projectRoot, ec);
    if (ec) {
        return std::unexpected(
            invalid("insomnia import: cannot canonicalise project root: " + ec.message()));
    }
    const auto canonStr = canonical.lexically_normal().string();
    const auto rootStr = root.lexically_normal().string();
    const bool contained = canonStr.size() >= rootStr.size() &&
                           canonStr.substr(0, rootStr.size()) == rootStr &&
                           (canonStr.size() == rootStr.size() || canonStr[rootStr.size()] == '/' ||
                            canonStr[rootStr.size()] == fs::path::preferred_separator);
    if (!contained) {
        return std::unexpected(invalid(
            "insomnia import: export path resolves outside the project root (" + rootStr + ")"));
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

/// Trim ASCII whitespace and strip Insomnia's `_.` template prefix so a raw
/// `{{ _.base_url }}` reference reduces to the bare variable name `base_url`.
std::string_view normalizeVarName(std::string_view inner) {
    while (!inner.empty() && (inner.front() == ' ' || inner.front() == '\t')) {
        inner.remove_prefix(1);
    }
    while (!inner.empty() && (inner.back() == ' ' || inner.back() == '\t')) {
        inner.remove_suffix(1);
    }
    if (inner.starts_with("_.")) {
        inner.remove_prefix(2);
    }
    return inner;
}

/// Rewrite Insomnia `{{ _.var }}` / `{{var}}` references to reqloom's
/// `{{env.var}}` scope. Already-scoped names (containing '.') pass through.
/// Nunjucks tags `{% ... %}` are recorded in `nunjucks` and left verbatim.
std::string rewriteTokens(std::string_view in,
                          std::set<std::string>& referenced,
                          std::set<std::string>& nunjucks) {
    std::string out;
    out.reserve(in.size());
    std::size_t i = 0;
    while (i < in.size()) {
        const auto tag = in.find("{%", i);
        const auto var = in.find("{{", i);
        const auto open = std::min(tag, var);
        if (open == std::string_view::npos) {
            out.append(in.substr(i));
            break;
        }
        out.append(in.substr(i, open - i));
        if (open == tag) {
            const auto close = in.find("%}", open + 2);
            if (close == std::string_view::npos) {
                out.append(in.substr(open));
                break;
            }
            const std::string_view body = in.substr(open + 2, close - (open + 2));
            nunjucks.emplace(std::string{body});
            out.append(in.substr(open, close + 2 - open));
            i = close + 2;
            continue;
        }
        const auto close = in.find("}}", open + 2);
        if (close == std::string_view::npos) {
            out.append(in.substr(open));
            break;
        }
        const std::string_view name = normalizeVarName(in.substr(open + 2, close - (open + 2)));
        if (name.empty()) {
            out.append("{{}}");
        } else if (name.find('.') != std::string_view::npos) {
            out.append("{{").append(name).append("}}");
        } else {
            referenced.emplace(name);
            out.append("{{env.").append(name).append("}}");
        }
        i = close + 2;
    }
    return out;
}

struct SplitUrl {
    std::string base;
    std::string path;
};

/// Split a raw URL into base (scheme+authority, or a resolved leading variable)
/// and path. Query/fragment are dropped — Insomnia keeps params separately.
SplitUrl splitRawUrl(std::string_view raw, const std::map<std::string, std::string>& vars) {
    auto end = raw.find_first_of("?#");
    std::string_view noQuery = raw.substr(0, end == std::string_view::npos ? raw.size() : end);

    SplitUrl out;
    if (noQuery.starts_with("{{")) {
        const auto close = noQuery.find("}}");
        if (close != std::string_view::npos) {
            const std::string varName{normalizeVarName(noQuery.substr(2, close - 2))};
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

}  // namespace

std::expected<ImportFromInsomnia::Outcome, ReqloomError> ImportFromInsomnia::run(
    const fs::path& exportFile, const fs::path& projectRoot) const {
    const auto pathOr = canonicalPath(exportFile, projectRoot);
    if (!pathOr) {
        return std::unexpected(pathOr.error());
    }

    std::error_code ec;
    const auto size = fs::file_size(*pathOr, ec);
    if (ec) {
        return std::unexpected(
            invalid("insomnia import: cannot stat export file: " + ec.message()));
    }
    if (size > kMaxExportBytes) {
        return std::unexpected(invalid(std::format(
            "insomnia import: export is too large ({} bytes; limit {})", size, kMaxExportBytes)));
    }

    std::ifstream in(*pathOr, std::ios::binary);
    if (!in) {
        return std::unexpected(invalid("insomnia import: cannot open export file"));
    }
    std::ostringstream ss;
    ss << in.rdbuf();

    json doc;
    try {
        doc = json::parse(ss.str());
    } catch (const json::parse_error& e) {
        return std::unexpected(
            ReqloomError{ErrorCode::YamlParse,
                         ErrorClass::Schema,
                         std::string{"insomnia import: invalid JSON: "} + e.what()});
    }

    if (!doc.is_object() || !doc.contains("resources") || !doc["resources"].is_array()) {
        return std::unexpected(
            invalid("insomnia import: not an Insomnia v4 export (missing resources array)"));
    }
    const auto exportType = jsonStr(doc, "_type");
    const bool hasFormat = doc.contains("__export_format");
    if (!hasFormat && exportType != "export") {
        return std::unexpected(
            invalid("insomnia import: not an Insomnia export (missing __export_format)"));
    }

    Outcome outcome;
    std::vector<std::string> warnings;

    const json& resources = doc["resources"];

    // Index nodes by _id and note the workspace(s). Insomnia references parents
    // by parentId, so we need random access to climb the group chain.
    std::unordered_map<std::string, const json*> byId;
    std::set<std::string> workspaceIds;
    for (const auto& node : resources) {
        if (!node.is_object()) {
            continue;
        }
        const auto id = jsonStr(node, "_id");
        if (!id.empty()) {
            byId[id] = &node;
        }
        if (jsonStr(node, "_type") == "workspace") {
            workspaceIds.insert(id);
        }
    }

    const std::string workspaceName = [&]() -> std::string {
        for (const auto& node : resources) {
            if (node.is_object() && jsonStr(node, "_type") == "workspace") {
                return jsonStr(node, "name");
            }
        }
        return {};
    }();
    outcome.project.name = workspaceName.empty() ? "Imported Collection" : workspaceName;
    outcome.project.defaultEnvironment = "default";
    auto& env = outcome.project.environments["default"];

    std::set<std::string> referencedVars;
    std::set<std::string> nunjucksTags;
    auto rewrite = [&](std::string_view s) {
        return rewriteTokens(s, referencedVars, nunjucksTags);
    };

    // Environment `data` blocks seed the default environment. Later (more
    // specific / sub-)environments overwrite earlier keys — good enough for a
    // single flattened env.  one merged env; per-env sub-scopes would
    // need Insomnia's environment tree preserved.
    for (const auto& node : resources) {
        if (node.is_object() && jsonStr(node, "_type") == "environment" && node.contains("data") &&
            node["data"].is_object()) {
            for (const auto& [key, value] : node["data"].items()) {
                if (value.is_string()) {
                    env[key] = rewrite(value.get<std::string>());
                } else if (!value.is_null()) {
                    env[key] = value.dump();
                }
            }
        }
    }

    // Resolve a request's owning resource id: climb parentId until the child of
    // a workspace. The top-most request_group in that chain is the resource;
    // requests directly under the workspace fall back to a default resource.
    const std::string defaultResource = sanitizeId(workspaceName, "requests");
    auto resourceForRequest = [&](const json& request) -> std::string {
        std::string topGroupName;
        std::string parentId = jsonStr(request, "parentId");
        int guard = 0;
        while (!parentId.empty() && guard++ < 256) {
            if (workspaceIds.contains(parentId)) {
                break;
            }
            const auto it = byId.find(parentId);
            if (it == byId.end()) {
                break;
            }
            const json& parent = *it->second;
            if (jsonStr(parent, "_type") == "request_group") {
                topGroupName = jsonStr(parent, "name");  // keep climbing → top-most wins
            }
            parentId = jsonStr(parent, "parentId");
        }
        return topGroupName.empty() ? defaultResource : sanitizeId(topGroupName, defaultResource);
    };

    const auto importedAt = nowIso8601Utc();
    std::set<std::string> seenOpIds;
    std::string projectBase;
    bool multipleBasesWarned = false;

    for (const auto& item : resources) {
        if (!item.is_object() || jsonStr(item, "_type") != "request") {
            continue;
        }

        const std::string resourceId = resourceForRequest(item);
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

        Operation op;
        op.id = OperationId{std::format("{}.{}", resourceId, unique)};
        op.resource = ResourceId{resourceId};
        op.method = parseMethod(jsonStr(item, "method").empty() ? "get" : jsonStr(item, "method"));

        const auto split = splitRawUrl(jsonStr(item, "url"), env);
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

        // Query params.
        if (item.contains("parameters") && item["parameters"].is_array()) {
            for (const auto& q : item["parameters"]) {
                if (q.value("disabled", false)) {
                    continue;
                }
                const auto key = jsonStr(q, "name");
                if (!key.empty()) {
                    op.queryParams[key] = rewrite(jsonStr(q, "value"));
                }
            }
        }

        // Headers.
        if (item.contains("headers") && item["headers"].is_array()) {
            for (const auto& h : item["headers"]) {
                if (h.value("disabled", false)) {
                    continue;
                }
                const auto key = jsonStr(h, "name");
                if (!key.empty()) {
                    op.headers[key] = rewrite(jsonStr(h, "value"));
                }
            }
        }

        // Body.
        if (item.contains("body") && item["body"].is_object()) {
            const json& body = item["body"];
            const auto mime = jsonStr(body, "mimeType");
            const auto text = jsonStr(body, "text");
            if (!text.empty()) {
                op.bodyTemplate = rewrite(text);
            } else if ((mime == "application/x-www-form-urlencoded" ||
                        mime == "multipart/form-data") &&
                       body.contains("params") && body["params"].is_array()) {
                std::map<std::string, std::string> form;
                for (const auto& f : body["params"]) {
                    if (f.value("disabled", false)) {
                        continue;
                    }
                    const auto key = jsonStr(f, "name");
                    if (key.empty()) {
                        continue;
                    }
                    if (jsonStr(f, "type") == "file") {
                        // Insomnia stores the picked file under `fileName`. Map
                        // to reqloom's `@path` multipart convention (empty → a
                        // `@` placeholder the editor shows as "Choose a file…").
                        const auto fileName = jsonStr(f, "fileName");
                        form[key] = fileName.empty() ? "@" : "@" + rewrite(fileName);
                    } else {
                        form[key] = rewrite(jsonStr(f, "value"));
                    }
                }
                if (!form.empty()) {
                    op.bodyForm = std::move(form);
                }
            }
        }

        Provenance prov;
        prov.source = Provenance::Source::InsomniaImport;
        prov.verifiedAgainst = Provenance::VerifiedAgainst::None;
        prov.importedAt = importedAt;
        if (!reqName.empty()) {
            prov.evidence["insomnia_name"] = reqName;
        }
        if (item.contains("authentication") && item["authentication"].is_object() &&
            !item["authentication"].empty()) {
            warnings.push_back(std::format(
                "operation {}: request-level auth was not imported — create an actor and attach it",
                op.id.value));
        }
        op.provenance = std::move(prov);

        resource.operations.emplace(unique, std::move(op));
    }

    if (outcome.project.resources.empty()) {
        return std::unexpected(invalid("insomnia import: export yielded zero importable requests"));
    }

    if (!projectBase.empty()) {
        env["baseUrl"] = projectBase;
    } else if (!env.contains("baseUrl")) {
        warnings.push_back(
            "no host could be derived from the requests — set `baseUrl` in the environment");
    }

    for (const auto& ref : referencedVars) {
        if (!env.contains(ref)) {
            env[ref] = "";
            warnings.push_back(std::format(
                "variable `{{{{{}}}}}` is referenced but was not defined — set it in the "
                "environment",
                ref));
        }
    }
    for (const auto& tag : nunjucksTags) {
        warnings.push_back(std::format(
            "Insomnia template tag `{{% {} %}}` has no reqloom equivalent — replace it manually",
            tag));
    }

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
