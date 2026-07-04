// ImportFromThunderClient — Thunder Client (VS Code) collection export (JSON) →
// Project parser. Sibling of ImportFromPostman.
//
// path/id/url helpers mirror ImportFromPostman rather than share a
// header (the existing importers do the same). Upgrade path: extract a shared
// ImportSupport.h once the duplication is worth removing.

#include "ImportFromThunderClient.h"

#include "../domain/DependencyResolver.h"

#include <nlohmann/json.hpp>

#include <algorithm>
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
            invalid("thunder client import: cannot canonicalise export path: " + ec.message()));
    }
    if (!fs::exists(canonical, ec) || !fs::is_regular_file(canonical, ec)) {
        return std::unexpected(
            invalid("thunder client import: export is not a regular file: " + canonical.string()));
    }
    auto root = fs::weakly_canonical(projectRoot, ec);
    if (ec) {
        return std::unexpected(
            invalid("thunder client import: cannot canonicalise project root: " + ec.message()));
    }
    const auto canonStr = canonical.lexically_normal().string();
    const auto rootStr = root.lexically_normal().string();
    const bool contained = canonStr.size() >= rootStr.size() &&
                           canonStr.substr(0, rootStr.size()) == rootStr &&
                           (canonStr.size() == rootStr.size() || canonStr[rootStr.size()] == '/' ||
                            canonStr[rootStr.size()] == fs::path::preferred_separator);
    if (!contained) {
        return std::unexpected(
            invalid("thunder client import: export path resolves outside the project root (" +
                    rootStr + ")"));
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

/// Rewrite bare `{{var}}` references to `{{env.var}}`. Already-scoped names
/// (containing '.') pass through; nothing here is treated as dynamic.
std::string rewriteTokens(std::string_view in, std::set<std::string>& referenced) {
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

}  // namespace

std::expected<ImportFromThunderClient::Outcome, ReqloomError> ImportFromThunderClient::run(
    const fs::path& exportFile, const fs::path& projectRoot) const {
    const auto pathOr = canonicalPath(exportFile, projectRoot);
    if (!pathOr) {
        return std::unexpected(pathOr.error());
    }

    std::error_code ec;
    const auto size = fs::file_size(*pathOr, ec);
    if (ec) {
        return std::unexpected(
            invalid("thunder client import: cannot stat export file: " + ec.message()));
    }
    if (size > kMaxExportBytes) {
        return std::unexpected(
            invalid(std::format("thunder client import: export is too large ({} bytes; limit {})",
                                size,
                                kMaxExportBytes)));
    }

    std::ifstream in(*pathOr, std::ios::binary);
    if (!in) {
        return std::unexpected(invalid("thunder client import: cannot open export file"));
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
                         std::string{"thunder client import: invalid JSON: "} + e.what()});
    }

    // Normalise to a list of collection objects. "Export Collection" gives an
    // array; a single object is also accepted.
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
    const bool anyRequests = std::any_of(collections.begin(), collections.end(), [](const json* c) {
        return c->contains("requests") && (*c)["requests"].is_array();
    });
    if (collections.empty() || !anyRequests) {
        return std::unexpected(
            invalid("thunder client import: not a Thunder Client collection (no requests array)"));
    }

    Outcome outcome;
    std::vector<std::string> warnings;

    const std::string collectionName = jsonStr(*collections.front(), "colName");
    outcome.project.name = collectionName.empty() ? "Imported Collection" : collectionName;
    outcome.project.defaultEnvironment = "default";
    auto& env = outcome.project.environments["default"];

    // Index folders by _id across all collections so containerId climbs resolve.
    std::unordered_map<std::string, const json*> folderById;
    for (const json* c : collections) {
        if (c->contains("folders") && (*c)["folders"].is_array()) {
            for (const auto& f : (*c)["folders"]) {
                const auto id = jsonStr(f, "_id");
                if (!id.empty()) {
                    folderById[id] = &f;
                }
            }
        }
    }

    const std::string defaultResource = sanitizeId(collectionName, "requests");
    auto resourceForRequest = [&](const json& request) -> std::string {
        std::string topFolderName;
        std::string containerId = jsonStr(request, "containerId");
        int guard = 0;
        while (!containerId.empty() && guard++ < 256) {
            const auto it = folderById.find(containerId);
            if (it == folderById.end()) {
                break;
            }
            const json& folder = *it->second;
            topFolderName = jsonStr(folder, "name");  // keep climbing → top-most wins
            containerId = jsonStr(folder, "containerId");
        }
        return topFolderName.empty() ? defaultResource : sanitizeId(topFolderName, defaultResource);
    };

    const auto importedAt = nowIso8601Utc();
    std::set<std::string> referencedVars;
    std::set<std::string> seenOpIds;
    std::string projectBase;
    bool multipleBasesWarned = false;
    auto rewrite = [&](std::string_view s) {
        return rewriteTokens(s, referencedVars);
    };

    for (const json* c : collections) {
        if (!c->contains("requests") || !(*c)["requests"].is_array()) {
            continue;
        }
        for (const auto& item : (*c)["requests"]) {
            if (!item.is_object()) {
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
            const auto methodRaw = jsonStr(item, "method");
            op.method = parseMethod(methodRaw.empty() ? "get" : methodRaw);

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

            // Query params — skip path variables (isPath) and disabled rows.
            if (item.contains("params") && item["params"].is_array()) {
                for (const auto& q : item["params"]) {
                    if (!q.is_object() || q.value("isPath", false) ||
                        q.value("isDisabled", false)) {
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
                    if (!h.is_object() || h.value("isDisabled", false)) {
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
                const auto type = jsonStr(body, "type");
                const auto raw = jsonStr(body, "raw");
                if (type == "formencoded" || type == "form") {
                    std::map<std::string, std::string> form;
                    if (body.contains("form") && body["form"].is_array()) {
                        for (const auto& f : body["form"]) {
                            if (!f.is_object() || f.value("isDisabled", false)) {
                                continue;
                            }
                            const auto key = jsonStr(f, "name");
                            if (!key.empty()) {
                                form[key] = rewrite(jsonStr(f, "value"));
                            }
                        }
                    }
                    if (body.contains("files") && body["files"].is_array()) {
                        for (const auto& f : body["files"]) {
                            const auto key = jsonStr(f, "name");
                            if (key.empty()) {
                                continue;
                            }
                            const auto path = jsonStr(f, "value");
                            form[key] = path.empty() ? "@" : "@" + rewrite(path);
                        }
                    }
                    if (!form.empty()) {
                        op.bodyForm = std::move(form);
                    }
                } else if (!raw.empty()) {
                    // json / text / xml / graphql all carry the payload in `raw`.
                    op.bodyTemplate = rewrite(raw);
                }
            }

            Provenance prov;
            prov.source = Provenance::Source::HandWritten;  // no TC-specific enum value
            prov.verifiedAgainst = Provenance::VerifiedAgainst::None;
            prov.importedAt = importedAt;
            prov.evidence["imported_from"] = "thunder_client";
            if (!reqName.empty()) {
                prov.evidence["thunder_name"] = reqName;
            }
            op.provenance = std::move(prov);

            resource.operations.emplace(unique, std::move(op));
        }
    }

    if (outcome.project.resources.empty()) {
        return std::unexpected(
            invalid("thunder client import: collection yielded zero importable requests"));
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
