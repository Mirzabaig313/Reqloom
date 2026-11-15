// ImportFromHttpie — HTTPie client export (JSON) → Project parser.
// Sibling of ImportFromPostman. Export shape (per HTTPie's export schema):
//   { "meta": {"format":"httpie","contentType":<...>}, "entry": <entry> }
// where entry is a Workspace / Collection / Request / Environment.
//
// ponytail: path/id/url helpers mirror the other importers rather than share a
// header. Upgrade path: extract a shared ImportSupport.h once worth it.

#include "ImportFromHttpie.h"
#include "ImportFromFileRefs.h"
#include "ImportFromJsonHelpers.h"
#include "ImportSupport.h"

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
#include <vector>

namespace reqloom::engine {

namespace {

namespace fs = std::filesystem;
namespace is = import_support;
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
            invalid("httpie import: cannot canonicalise export path: " + ec.message()));
    }
    if (!fs::exists(canonical, ec) || !fs::is_regular_file(canonical, ec)) {
        return std::unexpected(
            invalid("httpie import: export is not a regular file: " + canonical.string()));
    }
    auto root = fs::weakly_canonical(projectRoot, ec);
    if (ec) {
        return std::unexpected(
            invalid("httpie import: cannot canonicalise project root: " + ec.message()));
    }
    const auto canonStr = canonical.lexically_normal().string();
    const auto rootStr = root.lexically_normal().string();
    const bool contained = canonStr.size() >= rootStr.size() &&
                           canonStr.substr(0, rootStr.size()) == rootStr &&
                           (canonStr.size() == rootStr.size() || canonStr[rootStr.size()] == '/' ||
                            canonStr[rootStr.size()] == fs::path::preferred_separator);
    if (!contained) {
        return std::unexpected(invalid(
            "httpie import: export path resolves outside the project root (" + rootStr + ")"));
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

/// Rewrite HTTPie `{{var}}` references to `{{env.<key>}}`; a dotted name is
/// sanitised to an environment key.
std::string rewriteTokens(std::string_view in, is::VarRefs& referenced) {
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
        } else {
            out.append(is::envRef(inner, referenced));
        }
        i = close + 2;
    }
    return out;
}

struct SplitUrl {
    std::string base;
    std::string path;
    std::string baseVar;  ///< environment key of a leading `{{var}}` base, if any
};

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
            out.baseVar = is::envKey(name);
            if (const auto it = vars.find(out.baseVar); it != vars.end()) {
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

std::expected<ImportFromHttpie::Outcome, ReqloomError> ImportFromHttpie::run(
    const fs::path& exportFile, const fs::path& projectRoot) const {
    const auto pathOr = canonicalPath(exportFile, projectRoot);
    if (!pathOr) {
        return std::unexpected(pathOr.error());
    }

    std::error_code ec;
    const auto size = fs::file_size(*pathOr, ec);
    if (ec) {
        return std::unexpected(invalid("httpie import: cannot stat export file: " + ec.message()));
    }
    if (size > kMaxExportBytes) {
        return std::unexpected(invalid(std::format(
            "httpie import: export is too large ({} bytes; limit {})", size, kMaxExportBytes)));
    }

    std::ifstream in(*pathOr, std::ios::binary);
    if (!in) {
        return std::unexpected(invalid("httpie import: cannot open export file"));
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
                         std::string{"httpie import: invalid JSON: "} + e.what()});
    }

    if (!doc.is_object() || !doc.contains("meta") || !doc.contains("entry") ||
        jsonStr(doc["meta"], "format") != "httpie") {
        return std::unexpected(
            invalid("httpie import: not an HTTPie export (missing meta.format=\"httpie\")"));
    }

    const json& entry = doc["entry"];
    if (!entry.is_object()) {
        return std::unexpected(invalid("httpie import: `entry` must be an object"));
    }

    Outcome outcome;
    std::vector<std::string> warnings;
    auto& envs = outcome.project.environments;

    is::VarRefs referencedVars;
    std::set<std::string> seenOpIds;
    std::string projectBase;
    std::string projectBaseVar;
    bool multipleBasesWarned = false;
    const auto importedAt = nowIso8601Utc();
    auto rewrite = [&](std::string_view s) {
        return rewriteTokens(s, referencedVars);
    };

    // Each HTTPie environment becomes an environment; `isDefault` (else the first) is the default.
    auto absorbEnvironment = [&](const json& e) {
        is::VarMap vars;
        if (e.contains("variables") && e["variables"].is_array()) {
            for (const auto& v : e["variables"]) {
                const auto name = jsonStr(v, "name");
                if (!name.empty()) {
                    vars[is::envKey(name)] = rewrite(jsonStr(v, "value"));
                }
            }
        }
        const auto name = is::environmentName(jsonStr(e, "name"), envs);
        envs[name] = std::move(vars);
        if (outcome.project.defaultEnvironment.empty() ||
            import_json::boolOr(e, "isDefault", false)) {
            outcome.project.defaultEnvironment = name;
        }
    };
    const is::VarMap* env = nullptr;
    const auto ensureDefaultEnvironment = [&] {
        if (envs.empty()) {
            outcome.project.defaultEnvironment = "default";
            envs["default"];
        }
        env = &envs.at(outcome.project.defaultEnvironment);
    };

    // Build one operation from an HTTPie Request object.
    auto buildRequest = [&](const std::string& resourceId, const json& req) {
        auto& resource = outcome.project.resources[ResourceId{resourceId}];
        if (resource.id.value.empty()) {
            resource.id = ResourceId{resourceId};
        }

        const std::string reqName = jsonStr(req, "name");
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
        const auto methodRaw = jsonStr(req, "method");
        op.method = parseMethod(methodRaw.empty() ? "get" : methodRaw);

        const auto split = splitRawUrl(jsonStr(req, "url"), *env);
        if (!split.base.empty()) {
            if (projectBase.empty()) {
                projectBase = split.base;
                projectBaseVar = split.baseVar;
            } else if (projectBase != split.base && !multipleBasesWarned) {
                warnings.push_back(std::format(
                    "requests target more than one host; the project baseUrl was set to `{}` — "
                    "review operations that use a different host",
                    projectBase));
                multipleBasesWarned = true;
            }
        }
        op.pathTemplate = rewrite(split.path);

        // Headers + query params are List = [{name, value, enabled}].
        auto absorbList = [&](std::string_view key, std::map<std::string, std::string>& into) {
            const auto it = req.find(key);
            if (it != req.end() && it->is_array()) {
                for (const auto& item : *it) {
                    if (!item.is_object() || !import_json::boolOr(item, "enabled", true)) {
                        continue;
                    }
                    const auto name = jsonStr(item, "name");
                    if (!name.empty()) {
                        into[name] = rewrite(jsonStr(item, "value"));
                    }
                }
            }
        };
        absorbList("headers", op.headers);
        absorbList("queryParams", op.queryParams);

        // Body.
        if (req.contains("body") && req["body"].is_object()) {
            const json& body = req["body"];
            const auto type = jsonStr(body, "type");
            if (type == "text" && body.contains("text") && body["text"].is_object()) {
                op.bodyTemplate = rewrite(jsonStr(body["text"], "value"));
            } else if (type == "graphql" && body.contains("graphql") &&
                       body["graphql"].is_object()) {
                json gql;
                gql["query"] = jsonStr(body["graphql"], "query");
                const auto varsRaw = jsonStr(body["graphql"], "variables");
                if (!varsRaw.empty()) {
                    gql["variables"] = varsRaw;
                }
                op.bodyTemplate = rewrite(gql.dump());
            } else if (type == "form" && body.contains("form") && body["form"].is_object() &&
                       body["form"].contains("fields") && body["form"]["fields"].is_array()) {
                std::map<std::string, std::string> form;
                for (const auto& f : body["form"]["fields"]) {
                    if (!f.is_object() || !import_json::boolOr(f, "enabled", true)) {
                        continue;
                    }
                    const auto name = jsonStr(f, "name");
                    if (name.empty()) {
                        continue;
                    }
                    const auto fieldType = jsonStr(f, "type");
                    if (fieldType == "file" || fieldType == "filetext") {
                        const auto fileName =
                            f.contains("file") ? jsonStr(f["file"], "name") : std::string{};
                        form[name] = import_files::fileFieldValue(
                            fileName, op.id.value, name, warnings, rewrite);
                    } else {
                        form[name] = import_files::textFieldValue(
                            jsonStr(f, "value"), op.id.value, name, warnings, rewrite);
                    }
                }
                if (!form.empty()) {
                    op.bodyForm = std::move(form);
                }
            } else if (type == "file") {
                warnings.push_back(std::format(
                    "operation {}: a raw file body was not imported — attach it in the form editor",
                    op.id.value));
            }
        }

        Provenance prov;
        prov.source = Provenance::Source::HandWritten;  // no HTTPie-specific enum value
        prov.verifiedAgainst = Provenance::VerifiedAgainst::None;
        prov.importedAt = importedAt;
        prov.evidence["imported_from"] = "httpie";
        if (!reqName.empty()) {
            prov.evidence["httpie_name"] = reqName;
        }
        is::warnDroppedAuth(warnings,
                            "operation " + op.id.value,
                            req.contains("auth") ? jsonStr(req["auth"], "type") : std::string{});
        op.provenance = std::move(prov);

        resource.operations.emplace(unique, std::move(op));
    };

    // A collection becomes a resource named after it; its requests are ops.
    auto absorbCollection = [&](const json& col) {
        const std::string resourceId = sanitizeId(jsonStr(col, "name"), "requests");
        if (col.contains("auth")) {
            is::warnDroppedAuth(warnings,
                                std::format("collection `{}`", jsonStr(col, "name")),
                                jsonStr(col["auth"], "type"));
        }
        if (col.contains("requests") && col["requests"].is_array()) {
            for (const auto& req : col["requests"]) {
                if (req.is_object()) {
                    buildRequest(resourceId, req);
                }
            }
        }
    };

    const auto contentType = jsonStr(doc["meta"], "contentType");
    if (contentType == "workspace" || entry.contains("collections")) {
        outcome.project.name =
            jsonStr(entry, "name").empty() ? "Imported Workspace" : jsonStr(entry, "name");
        if (entry.contains("environments") && entry["environments"].is_array()) {
            for (const auto& e : entry["environments"]) {
                if (e.is_object()) {
                    absorbEnvironment(e);
                }
            }
        }
        ensureDefaultEnvironment();
        if (entry.contains("collections") && entry["collections"].is_array()) {
            for (const auto& col : entry["collections"]) {
                if (col.is_object()) {
                    absorbCollection(col);
                }
            }
        }
        if (entry.contains("drafts") && entry["drafts"].is_array()) {
            for (const auto& req : entry["drafts"]) {
                if (req.is_object()) {
                    buildRequest("drafts", req);
                }
            }
        }
    } else if (contentType == "collection" || entry.contains("requests")) {
        outcome.project.name =
            jsonStr(entry, "name").empty() ? "Imported Collection" : jsonStr(entry, "name");
        ensureDefaultEnvironment();
        absorbCollection(entry);
    } else if (contentType == "environment" || entry.contains("variables")) {
        outcome.project.name =
            jsonStr(entry, "name").empty() ? "Imported Environment" : jsonStr(entry, "name");
        absorbEnvironment(entry);
        ensureDefaultEnvironment();
    } else {
        // A bare Request entry.
        outcome.project.name =
            jsonStr(entry, "name").empty() ? "Imported Request" : jsonStr(entry, "name");
        ensureDefaultEnvironment();
        buildRequest("requests", entry);
    }

    if (outcome.project.resources.empty()) {
        return std::unexpected(invalid("httpie import: export yielded zero importable requests"));
    }

    if (!projectBase.empty()) {
        is::applyBaseUrl(envs, projectBase, projectBaseVar);
    } else if (!env->contains("baseUrl")) {
        warnings.push_back(
            "no host could be derived from the requests — set `baseUrl` in the environment");
    }

    is::warnRenamedVars(referencedVars, warnings);
    for (const auto& ref : referencedVars.referenced) {
        if (is::seedMissing(envs, ref)) {
            warnings.push_back(std::format(
                "variable `{{{{{}}}}}` is referenced but was not defined — set it in the "
                "environment",
                ref));
        }
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
