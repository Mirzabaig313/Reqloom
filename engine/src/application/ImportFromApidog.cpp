// ImportFromApidog — Apidog native export (JSON) → Project parser.
// Sibling of ImportFromPostman.
//
// ponytail: path/id/token helpers mirror the other importers rather than share
// a header. Upgrade path: extract a shared ImportSupport.h once worth it.

#include "ImportFromApidog.h"

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

constexpr std::uintmax_t kMaxExportBytes = std::uintmax_t{32} * 1024 * 1024;
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
            invalid("apidog import: cannot canonicalise export path: " + ec.message()));
    }
    if (!fs::exists(canonical, ec) || !fs::is_regular_file(canonical, ec)) {
        return std::unexpected(
            invalid("apidog import: export is not a regular file: " + canonical.string()));
    }
    auto root = fs::weakly_canonical(projectRoot, ec);
    if (ec) {
        return std::unexpected(
            invalid("apidog import: cannot canonicalise project root: " + ec.message()));
    }
    const auto canonStr = canonical.lexically_normal().string();
    const auto rootStr = root.lexically_normal().string();
    const bool contained = canonStr.size() >= rootStr.size() &&
                           canonStr.substr(0, rootStr.size()) == rootStr &&
                           (canonStr.size() == rootStr.size() || canonStr[rootStr.size()] == '/' ||
                            canonStr[rootStr.size()] == fs::path::preferred_separator);
    if (!contained) {
        return std::unexpected(invalid(
            "apidog import: export path resolves outside the project root (" + rootStr + ")"));
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
           ct.find("multipart/form-data") != std::string_view::npos ||
           ct.find("form-data") != std::string_view::npos;
}

}  // namespace

std::expected<ImportFromApidog::Outcome, ReqloomError> ImportFromApidog::run(
    const fs::path& exportFile, const fs::path& projectRoot) const {
    const auto pathOr = canonicalPath(exportFile, projectRoot);
    if (!pathOr) {
        return std::unexpected(pathOr.error());
    }

    std::error_code ec;
    const auto size = fs::file_size(*pathOr, ec);
    if (ec) {
        return std::unexpected(invalid("apidog import: cannot stat export file: " + ec.message()));
    }
    if (size > kMaxExportBytes) {
        return std::unexpected(invalid(std::format(
            "apidog import: export is too large ({} bytes; limit {})", size, kMaxExportBytes)));
    }

    std::ifstream in(*pathOr, std::ios::binary);
    if (!in) {
        return std::unexpected(invalid("apidog import: cannot open export file"));
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
                         std::string{"apidog import: invalid JSON: "} + e.what()});
    }

    if (!doc.is_object() || !doc.contains("apidogProject") || !doc.contains("apiCollection") ||
        !doc["apiCollection"].is_array()) {
        return std::unexpected(
            invalid("apidog import: not an Apidog export (missing apidogProject/apiCollection)"));
    }

    Outcome outcome;
    std::vector<std::string> warnings;
    const std::string projectName =
        doc.contains("info") ? jsonStr(doc["info"], "name") : std::string{};
    outcome.project.name = projectName.empty() ? "Imported Project" : projectName;
    outcome.project.defaultEnvironment = "default";
    auto& env = outcome.project.environments["default"];

    const std::string defaultResource = sanitizeId(projectName, "requests");
    std::set<std::string> referencedVars;
    std::set<std::string> seenOpIds;
    const auto importedAt = nowIso8601Utc();
    auto rewrite = [&](std::string_view s) {
        return rewriteTokens(s, referencedVars);
    };

    auto buildEndpoint = [&](const std::string& resourceId, const json& item) {
        const json& api = item["api"];
        if (!api.is_object()) {
            return;
        }
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
        const auto methodRaw = jsonStr(api, "method");
        op.method = parseMethod(methodRaw.empty() ? "get" : methodRaw);
        op.pathTemplate = rewrite(jsonStr(api, "path"));

        // Parameters: header + query lists of {name, example, enable}.
        if (api.contains("parameters") && api["parameters"].is_object()) {
            const json& params = api["parameters"];
            auto absorb = [&](std::string_view key, std::map<std::string, std::string>& into) {
                const auto it = params.find(key);
                if (it != params.end() && it->is_array()) {
                    for (const auto& p : *it) {
                        if (!p.is_object() || !p.value("enable", true)) {
                            continue;
                        }
                        const auto name = jsonStr(p, "name");
                        if (!name.empty()) {
                            into[name] = rewrite(jsonStr(p, "example"));
                        }
                    }
                }
            };
            absorb("header", op.headers);
            absorb("query", op.queryParams);
        }

        // Request body: prefer a concrete example; else a form parameter list.
        if (api.contains("requestBody") && api["requestBody"].is_object()) {
            const json& body = api["requestBody"];
            const auto type = jsonStr(body, "type");
            std::string exampleValue;
            if (body.contains("examples") && body["examples"].is_array() &&
                !body["examples"].empty()) {
                exampleValue = jsonStr(body["examples"].front(), "value");
            }
            if (isFormContentType(type) && body.contains("parameters") &&
                body["parameters"].is_array()) {
                std::map<std::string, std::string> form;
                for (const auto& f : body["parameters"]) {
                    if (!f.is_object() || !f.value("enable", true)) {
                        continue;
                    }
                    const auto name = jsonStr(f, "name");
                    if (name.empty()) {
                        continue;
                    }
                    if (jsonStr(f, "type") == "file") {
                        form[name] = "@";
                    } else {
                        form[name] = rewrite(jsonStr(f, "example"));
                    }
                }
                if (!form.empty()) {
                    op.bodyForm = std::move(form);
                }
            } else if (!exampleValue.empty()) {
                op.bodyTemplate = rewrite(exampleValue);
            }
        }

        // Expected status from the first declared response code.
        if (api.contains("responses") && api["responses"].is_array() && !api["responses"].empty()) {
            const auto& first = api["responses"].front();
            if (first.is_object() && first.contains("code") && first["code"].is_number_integer()) {
                op.expectStatus = first["code"].get<int>();
            }
        }

        Provenance prov;
        prov.source = Provenance::Source::HandWritten;  // no Apidog-specific enum value
        prov.verifiedAgainst = Provenance::VerifiedAgainst::None;
        prov.importedAt = importedAt;
        prov.evidence["imported_from"] = "apidog";
        if (!reqName.empty()) {
            prov.evidence["apidog_name"] = reqName;
        }
        op.provenance = std::move(prov);

        resource.operations.emplace(unique, std::move(op));
    };

    // Walk a folder's `items`: sub-folders (which carry `items`) or endpoints
    // (which carry `api`). A top-level folder becomes a resource; deeper folders
    // fold into their top-level ancestor.
    auto walk =
        [&](const json& folder, const std::string& resourceId, int depth, auto&& self) -> void {
        if (depth > kMaxItemDepth) {
            warnings.push_back(
                "folder nesting exceeded the supported depth; deeper items were skipped");
            return;
        }
        const auto itemsIt = folder.find("items");
        if (itemsIt == folder.end() || !itemsIt->is_array()) {
            return;
        }
        for (const auto& item : *itemsIt) {
            if (!item.is_object()) {
                continue;
            }
            if (item.contains("items") && item["items"].is_array()) {
                const std::string childResource =
                    resourceId.empty() ? sanitizeId(jsonStr(item, "name"), defaultResource)
                                       : resourceId;
                self(item, childResource, depth + 1, self);
            } else if (item.contains("api")) {
                buildEndpoint(resourceId.empty() ? defaultResource : resourceId, item);
            }
        }
    };

    // Apidog wraps everything in a single synthetic top folder (usually "Root").
    // Unwrap it so its child folders become resources; a multi-folder top level
    // means each entry is itself a resource.
    const json& collection = doc["apiCollection"];
    if (collection.size() == 1) {
        walk(collection.front(), std::string{}, 0, walk);
    } else {
        for (const auto& top : collection) {
            if (top.is_object()) {
                walk(top, sanitizeId(jsonStr(top, "name"), defaultResource), 0, walk);
            }
        }
    }

    if (outcome.project.resources.empty()) {
        return std::unexpected(invalid("apidog import: export yielded zero importable endpoints"));
    }

    // Apidog native exports carry no server URL (it lives in environments that
    // aren't part of this file), so baseUrl can't be derived.
    if (!env.contains("baseUrl")) {
        warnings.push_back(
            "Apidog exports carry no server URL — set `baseUrl` in the environment before running");
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
