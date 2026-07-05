// ImportFromHoppscotch — Hoppscotch collection export (JSON) → Project parser.
// Sibling of ImportFromPostman.
//
// path/id/url helpers mirror ImportFromPostman rather than share a
// header (the existing importers do the same). Upgrade path: extract a shared
// ImportSupport.h once the duplication is worth removing.

#include "ImportFromHoppscotch.h"

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
using json = nlohmann::json;

constexpr std::uintmax_t kMaxExportBytes = std::uintmax_t{16} * 1024 * 1024;
constexpr int kMaxItemDepth = 64;

ReqloomError invalid(std::string detail) {
    return ReqloomError{ErrorCode::SchemaInvalid, ErrorClass::Schema, std::move(detail)};
}

[[nodiscard]] std::expected<fs::path, ReqloomError> canonicalPath(const fs::path& file,
                                                                  const fs::path& projectRoot) {
    std::error_code ec;
    auto canonical = fs::weakly_canonical(file, ec);
    if (ec) {
        return std::unexpected(
            invalid("hoppscotch import: cannot canonicalise export path: " + ec.message()));
    }
    if (!fs::exists(canonical, ec) || !fs::is_regular_file(canonical, ec)) {
        return std::unexpected(
            invalid("hoppscotch import: export is not a regular file: " + canonical.string()));
    }
    auto root = fs::weakly_canonical(projectRoot, ec);
    if (ec) {
        return std::unexpected(
            invalid("hoppscotch import: cannot canonicalise project root: " + ec.message()));
    }
    const auto canonStr = canonical.lexically_normal().string();
    const auto rootStr = root.lexically_normal().string();
    const bool contained = canonStr.size() >= rootStr.size() &&
                           canonStr.substr(0, rootStr.size()) == rootStr &&
                           (canonStr.size() == rootStr.size() || canonStr[rootStr.size()] == '/' ||
                            canonStr[rootStr.size()] == fs::path::preferred_separator);
    if (!contained) {
        return std::unexpected(invalid(
            "hoppscotch import: export path resolves outside the project root (" + rootStr + ")"));
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

/// Rewrite Hoppscotch `<<var>>` references to reqloom's `{{env.var}}` scope.
/// Already-scoped names (containing '.') pass through unwrapped.
std::string rewriteTokens(std::string_view in, std::set<std::string>& referenced) {
    std::string out;
    out.reserve(in.size());
    std::size_t i = 0;
    while (i < in.size()) {
        const auto open = in.find("<<", i);
        if (open == std::string_view::npos) {
            out.append(in.substr(i));
            break;
        }
        out.append(in.substr(i, open - i));
        const auto close = in.find(">>", open + 2);
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
            out.append("<<>>");
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

struct SplitUrl {
    std::string base;
    std::string path;
};

/// Split a raw endpoint into base + path. A leading `<<var>>` is treated as the
/// base placeholder — resolved from `vars` when present, else left empty so the
/// user fills `baseUrl` — with the remainder as the path.
SplitUrl splitRawUrl(std::string_view raw, const std::map<std::string, std::string>& vars) {
    auto end = raw.find_first_of("?#");
    std::string_view noQuery = raw.substr(0, end == std::string_view::npos ? raw.size() : end);

    SplitUrl out;
    if (noQuery.starts_with("<<")) {
        const auto close = noQuery.find(">>");
        if (close != std::string_view::npos) {
            std::string_view name = noQuery.substr(2, close - 2);
            while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) {
                name.remove_prefix(1);
            }
            while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
                name.remove_suffix(1);
            }
            if (const auto it = vars.find(std::string{name}); it != vars.end()) {
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

bool isFormContentType(std::string_view ct) {
    return ct.find("x-www-form-urlencoded") != std::string_view::npos ||
           ct.find("multipart/form-data") != std::string_view::npos;
}

}  // namespace

std::expected<ImportFromHoppscotch::Outcome, ReqloomError> ImportFromHoppscotch::run(
    const fs::path& exportFile, const fs::path& projectRoot) const {
    const auto pathOr = canonicalPath(exportFile, projectRoot);
    if (!pathOr) {
        return std::unexpected(pathOr.error());
    }

    std::error_code ec;
    const auto size = fs::file_size(*pathOr, ec);
    if (ec) {
        return std::unexpected(
            invalid("hoppscotch import: cannot stat export file: " + ec.message()));
    }
    if (size > kMaxExportBytes) {
        return std::unexpected(invalid(std::format(
            "hoppscotch import: export is too large ({} bytes; limit {})", size, kMaxExportBytes)));
    }

    std::ifstream in(*pathOr, std::ios::binary);
    if (!in) {
        return std::unexpected(invalid("hoppscotch import: cannot open export file"));
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
                         std::string{"hoppscotch import: invalid JSON: "} + e.what()});
    }

    // Hoppscotch exports a single collection object (or, from "export all", an
    // array of them). A collection has `requests` and/or `folders`.
    std::vector<const json*> collections;
    if (doc.is_array()) {
        for (const auto& c : doc) {
            if (c.is_object()) {
                collections.push_back(&c);
            }
        }
    } else if (doc.is_object()) {
        collections.push_back(&doc);
    }
    const auto hasRequestsOrFolders = [](const json* c) {
        return (c->contains("requests") && (*c)["requests"].is_array()) ||
               (c->contains("folders") && (*c)["folders"].is_array());
    };
    bool anyValid = false;
    for (const json* c : collections) {
        if (hasRequestsOrFolders(c)) {
            anyValid = true;
            break;
        }
    }
    if (collections.empty() || !anyValid) {
        return std::unexpected(
            invalid("hoppscotch import: not a Hoppscotch collection (no requests/folders array)"));
    }

    Outcome outcome;
    std::vector<std::string> warnings;

    const std::string collectionName = jsonStr(*collections.front(), "name");
    outcome.project.name = collectionName.empty() ? "Imported Collection" : collectionName;
    outcome.project.defaultEnvironment = "default";
    auto& env = outcome.project.environments["default"];

    const std::string defaultResource = sanitizeId(collectionName, "requests");
    std::set<std::string> referencedVars;
    std::set<std::string> seenOpIds;
    std::string projectBase;
    bool multipleBasesWarned = false;
    const auto importedAt = nowIso8601Utc();
    auto rewrite = [&](std::string_view s) {
        return rewriteTokens(s, referencedVars);
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

        Operation op;
        op.id = OperationId{std::format("{}.{}", resourceId, unique)};
        op.resource = ResourceId{resourceId};
        const auto methodRaw = jsonStr(item, "method");
        op.method = parseMethod(methodRaw.empty() ? "get" : methodRaw);

        const auto split = splitRawUrl(jsonStr(item, "endpoint"), env);
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
        if (item.contains("params") && item["params"].is_array()) {
            for (const auto& q : item["params"]) {
                if (!q.is_object() || !q.value("active", true)) {
                    continue;
                }
                const auto key = jsonStr(q, "key");
                if (!key.empty()) {
                    op.queryParams[key] = rewrite(jsonStr(q, "value"));
                }
            }
        }

        // Headers.
        if (item.contains("headers") && item["headers"].is_array()) {
            for (const auto& h : item["headers"]) {
                if (!h.is_object() || !h.value("active", true)) {
                    continue;
                }
                const auto key = jsonStr(h, "key");
                if (!key.empty()) {
                    op.headers[key] = rewrite(jsonStr(h, "value"));
                }
            }
        }

        // Body. Newer Hoppscotch: body{contentType, body}. `body` is a string
        // for raw types; for form types it's an array of {key,value,active,isFile}.
        if (item.contains("body") && item["body"].is_object()) {
            const json& body = item["body"];
            const auto contentType = jsonStr(body, "contentType");
            const auto payloadIt = body.find("body");
            if (payloadIt != body.end()) {
                if (payloadIt->is_string() && !payloadIt->get<std::string>().empty()) {
                    op.bodyTemplate = rewrite(payloadIt->get<std::string>());
                } else if (payloadIt->is_array() && isFormContentType(contentType)) {
                    std::map<std::string, std::string> form;
                    for (const auto& f : *payloadIt) {
                        if (!f.is_object() || !f.value("active", true)) {
                            continue;
                        }
                        const auto key = jsonStr(f, "key");
                        if (key.empty()) {
                            continue;
                        }
                        if (f.value("isFile", false)) {
                            const auto val = jsonStr(f, "value");
                            form[key] = val.empty() ? "@" : "@" + rewrite(val);
                        } else {
                            form[key] = rewrite(jsonStr(f, "value"));
                        }
                    }
                    if (!form.empty()) {
                        op.bodyForm = std::move(form);
                    }
                }
            }
        }

        Provenance prov;
        prov.source = Provenance::Source::HandWritten;  // no Hoppscotch-specific enum value
        prov.verifiedAgainst = Provenance::VerifiedAgainst::None;
        prov.importedAt = importedAt;
        prov.evidence["imported_from"] = "hoppscotch";
        if (!reqName.empty()) {
            prov.evidence["hoppscotch_name"] = reqName;
        }
        op.provenance = std::move(prov);

        resource.operations.emplace(unique, std::move(op));
    };

    // Recursive walk: a top-level folder becomes its own resource; nested
    // folders fold into the ancestor resource; root requests land in a resource
    // named after the collection.
    auto walk =
        [&](const json& node, const std::string& resourceId, int depth, auto&& self) -> void {
        if (depth > kMaxItemDepth) {
            warnings.push_back(
                "folder nesting exceeded the supported depth; deeper items were skipped");
            return;
        }
        if (node.contains("requests") && node["requests"].is_array()) {
            for (const auto& req : node["requests"]) {
                if (req.is_object()) {
                    buildRequest(resourceId.empty() ? defaultResource : resourceId, req);
                }
            }
        }
        if (node.contains("folders") && node["folders"].is_array()) {
            for (const auto& folder : node["folders"]) {
                if (!folder.is_object()) {
                    continue;
                }
                const std::string childResource =
                    resourceId.empty() ? sanitizeId(jsonStr(folder, "name"), defaultResource)
                                       : resourceId;
                self(folder, childResource, depth + 1, self);
            }
        }
    };

    for (const json* c : collections) {
        walk(*c, std::string{}, 0, walk);
    }

    if (outcome.project.resources.empty()) {
        return std::unexpected(
            invalid("hoppscotch import: collection yielded zero importable requests"));
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
                "variable `<<{}>>` is referenced but was not defined — set it in the environment",
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
