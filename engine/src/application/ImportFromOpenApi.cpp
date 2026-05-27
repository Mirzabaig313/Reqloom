// ImportFromOpenApi — OpenAPI 3.x → Project parser.
#include "ImportFromOpenApi.h"

#include "Verifier.h"

#include <yaml-cpp/yaml.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace chainapi::engine {

namespace {

namespace fs = std::filesystem;

ChainApiError invalid(std::string detail) {
    return ChainApiError{ErrorCode::SchemaInvalid, ErrorClass::Schema, std::move(detail)};
}

// Containment check: resolved path must live under projectRoot with a
// path-separator boundary so `/home/user/proj` doesn't admit `/home/user/proj-evil`.
std::expected<fs::path, ChainApiError> canonicalSpecPath(const fs::path& spec,
                                                         const fs::path& projectRoot) {
    std::error_code ec;
    auto canonical = fs::weakly_canonical(spec, ec);
    if (ec) {
        return std::unexpected(
            invalid("openapi import: cannot canonicalise spec path: " + ec.message()));
    }
    if (!fs::exists(canonical, ec) || !fs::is_regular_file(canonical, ec)) {
        return std::unexpected(
            invalid("openapi import: spec is not a regular file: " + canonical.string()));
    }

    auto root = fs::weakly_canonical(projectRoot, ec);
    if (ec) {
        return std::unexpected(
            invalid("openapi import: cannot canonicalise project root: " + ec.message()));
    }

    const auto canonStr = canonical.lexically_normal().string();
    const auto rootStr = root.lexically_normal().string();
    const bool contained = canonStr.size() >= rootStr.size() &&
                           canonStr.substr(0, rootStr.size()) == rootStr &&
                           (canonStr.size() == rootStr.size() || canonStr[rootStr.size()] == '/' ||
                            canonStr[rootStr.size()] == fs::path::preferred_separator);
    if (!contained) {
        return std::unexpected(invalid(
            "openapi import: spec path resolves outside the project root (" + rootStr + ")"));
    }

    return canonical;
}

std::string toLowerAscii(std::string_view s) {
    std::string out{s};
    for (auto& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return out;
}

struct SingulariseResult {
    std::string value;
    bool modified{false};
};

SingulariseResult singulariseDetailed(std::string_view word) {
    if (word.size() > 3 && word.ends_with("ies")) {
        return {std::string{word.substr(0, word.size() - 3)} + "y", true};
    }
    if (word.size() > 2 && word.ends_with("es") &&
        (word.ends_with("ches") || word.ends_with("shes") || word.ends_with("xes") ||
         word.ends_with("ses"))) {
        return {std::string{word.substr(0, word.size() - 2)}, true};
    }
    if (word.size() > 1 && word.ends_with("s") && !word.ends_with("ss")) {
        return {std::string{word.substr(0, word.size() - 1)}, true};
    }
    return {std::string{word}, false};
}

std::vector<std::string> splitPathSegments(std::string_view path) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < path.size()) {
        const auto next = path.find('/', i);
        const auto piece =
            path.substr(i, next == std::string_view::npos ? std::string_view::npos : next - i);
        if (!piece.empty()) out.emplace_back(piece);
        if (next == std::string_view::npos) break;
        i = next + 1;
    }
    return out;
}

struct ResourceDerivation {
    std::string id;
    std::string fromSegment;
    bool singularised{false};
};

ResourceDerivation deriveResourceIdDetailed(std::string_view path) {
    const auto segments = splitPathSegments(path);
    for (auto it = segments.rbegin(); it != segments.rend(); ++it) {
        if (it->starts_with("{") || it->starts_with(":")) continue;
        const auto lowered = toLowerAscii(*it);
        const auto sing = singulariseDetailed(lowered);
        return {sing.value, *it, sing.modified};
    }
    return {"root", "", false};
}

std::string deriveOperationName(std::string_view methodLower, std::string_view path) {
    const auto segments = splitPathSegments(path);
    const bool endsWithParam =
        !segments.empty() && (segments.back().starts_with("{") || segments.back().starts_with(":"));

    if (methodLower == "get") return endsWithParam ? "get" : "list";
    if (methodLower == "post") return "create";
    if (methodLower == "put") return "update";
    if (methodLower == "patch") return "patch";
    if (methodLower == "delete") return "delete";
    if (methodLower == "head") return "head";
    if (methodLower == "options") return "options";
    return std::string{methodLower};
}

HttpMethod parseHttpMethod(std::string_view methodLower) {
    if (methodLower == "post") return HttpMethod::Post;
    if (methodLower == "put") return HttpMethod::Put;
    if (methodLower == "patch") return HttpMethod::Patch;
    if (methodLower == "delete") return HttpMethod::Delete;
    if (methodLower == "head") return HttpMethod::Head;
    if (methodLower == "options") return HttpMethod::Options;
    return HttpMethod::Get;
}

constexpr bool isOpenApiMethod(std::string_view m) noexcept {
    return m == "get" || m == "post" || m == "put" || m == "patch" || m == "delete" ||
           m == "head" || m == "options";
}

// Pick the first 2xx status declared on the operation, falling back to 200.
int pickExpectedStatus(const YAML::Node& responses) {
    if (!responses || !responses.IsMap()) return 200;
    for (const auto& kv : responses) {
        const auto code = kv.first.as<std::string>("");
        if (code.size() == 3 && code[0] == '2') {
            try {
                return std::stoi(code);
            } catch (...) {
                return 200;
            }
        }
    }
    return 200;
}

std::string nowIso8601Utc() {
    using namespace std::chrono;
    const auto now = system_clock::to_time_t(system_clock::now());
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z",
                       tm.tm_year + 1900,
                       tm.tm_mon + 1,
                       tm.tm_mday,
                       tm.tm_hour,
                       tm.tm_min,
                       tm.tm_sec);
}

/// Server URL → environment baseUrl. Only `servers[0]` is consumed; later
/// slices can offer environment-per-server.
std::string firstServerUrl(const YAML::Node& root) {
    const auto& servers = root["servers"];
    if (!servers || !servers.IsSequence() || servers.size() == 0) return "/";
    const auto& first = servers[0];
    if (!first || !first.IsMap()) return "/";
    return first["url"].as<std::string>("/");
}

/// Rewrite `{name}` path parameters into `{{<resource>.<name>}}` template
/// references. The engine's variable resolver will then pull the value
/// from a previously-extracted upstream variable. Linking the upstream
/// op that *produces* the value is left to the user (or to an AI import
/// pass) — we surface a warning per parameter to make the gap visible.
struct PathRewriteResult {
    std::string rewritten;
    std::vector<std::string> paramNames;
};

PathRewriteResult rewritePathParams(std::string_view pathTemplate, std::string_view resourceId) {
    PathRewriteResult out;
    out.rewritten.reserve(pathTemplate.size());

    std::size_t i = 0;
    while (i < pathTemplate.size()) {
        const auto open = pathTemplate.find('{', i);
        if (open == std::string_view::npos) {
            out.rewritten.append(pathTemplate.substr(i));
            break;
        }
        out.rewritten.append(pathTemplate.substr(i, open - i));
        const auto close = pathTemplate.find('}', open);
        if (close == std::string_view::npos) {
            out.rewritten.append(pathTemplate.substr(open));
            break;
        }
        const auto param = pathTemplate.substr(open + 1, close - open - 1);
        out.paramNames.emplace_back(param);
        out.rewritten.append("{{");
        out.rewritten.append(resourceId);
        out.rewritten.push_back('.');
        out.rewritten.append(param);
        out.rewritten.append("}}");
        i = close + 1;
    }
    return out;
}

YAML::Node firstJsonResponseSchema(const YAML::Node& opNode) {
    const auto& responses = opNode["responses"];
    if (!responses || !responses.IsMap()) return {};
    for (const auto& kv : responses) {
        const auto code = kv.first.as<std::string>("");
        if (code.size() != 3 || code[0] != '2') continue;
        const auto& resp = kv.second;
        if (!resp || !resp.IsMap()) continue;
        const auto& content = resp["content"];
        if (!content || !content.IsMap()) continue;
        const auto& json = content["application/json"];
        if (!json || !json.IsMap()) continue;
        return json["schema"];
    }
    return {};
}

std::optional<YAML::Node> firstJsonResponseExample(const YAML::Node& opNode) {
    const auto& responses = opNode["responses"];
    if (!responses || !responses.IsMap()) return std::nullopt;
    for (const auto& kv : responses) {
        const auto code = kv.first.as<std::string>("");
        if (code.size() != 3 || code[0] != '2') continue;
        const auto& resp = kv.second;
        if (!resp || !resp.IsMap()) continue;
        const auto& content = resp["content"];
        if (!content || !content.IsMap()) continue;
        const auto& json = content["application/json"];
        if (!json || !json.IsMap()) continue;
        if (const auto example = json["example"]; example.IsDefined() && !example.IsNull()) {
            return example;
        }
        if (const auto examples = json["examples"];
            examples.IsDefined() && examples.IsMap() && examples.size() > 0) {
            const auto first = examples.begin()->second;
            if (first.IsDefined() && first.IsMap() && first["value"].IsDefined()) {
                return first["value"];
            }
        }
    }
    return std::nullopt;
}

bool isScalarSchemaType(std::string_view t) noexcept {
    return t == "string" || t == "integer" || t == "number" || t == "boolean";
}

// YAML::Node's destructor is not marked noexcept upstream (yaml-cpp 0.8),
// so the implicit ~UnwrappedSchema is noexcept-suspect. In practice
// yaml-cpp's destructor only releases ref-counted nodes and cannot throw.
// Silenced locally rather than wrapping a noexcept dtor in try/catch.
// NOLINTNEXTLINE(bugprone-exception-escape)
struct UnwrappedSchema {
    YAML::Node schema;
    bool wrappedInData{false};
};

UnwrappedSchema unwrapSchema(const YAML::Node& schema) {
    UnwrappedSchema out{schema, false};
    if (!schema || !schema.IsMap()) return out;
    if (schema["type"].as<std::string>("") != "object") return out;
    const auto& props = schema["properties"];
    if (!props || !props.IsMap()) return out;
    if (props.size() == 1 && props["data"]) {
        const auto& inner = props["data"];
        if (inner && inner.IsMap() && inner["type"].as<std::string>("") == "object") {
            out.schema = inner;
            out.wrappedInData = true;
        }
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> inferExtractionsFromSchema(
    const YAML::Node& schemaIn) {
    std::vector<std::pair<std::string, std::string>> out;
    const auto unwrapped = unwrapSchema(schemaIn);
    const auto& schema = unwrapped.schema;
    if (!schema || !schema.IsMap()) return out;
    if (schema["type"].as<std::string>("") != "object") return out;

    const auto& props = schema["properties"];
    if (!props || !props.IsMap()) return out;

    const std::string root = unwrapped.wrappedInData ? "$.data." : "$.";

    for (const auto& kv : props) {
        const auto name = kv.first.as<std::string>("");
        const auto& propSchema = kv.second;
        if (name.empty() || !propSchema || !propSchema.IsMap()) continue;
        const auto type = propSchema["type"].as<std::string>("");
        if (!isScalarSchemaType(type)) continue;
        out.emplace_back(name, root + name);
    }
    return out;
}

constexpr int kMaxYamlToJsonDepth = 64;

nlohmann::json yamlToJson(const YAML::Node& node, int depth = 0) {
    if (depth > kMaxYamlToJsonDepth) return nlohmann::json{nullptr};
    if (!node || node.IsNull()) return nlohmann::json{nullptr};

    if (node.IsScalar()) {
        const auto raw = node.Scalar();
        if (raw == "true") return nlohmann::json(true);
        if (raw == "false") return nlohmann::json(false);
        if (raw == "null" || raw == "~") return nlohmann::json{nullptr};

        if (!raw.empty()) {
            const auto* first = raw.data();
            const auto* last = first + raw.size();
            long long asInt = 0;
            if (auto fc = std::from_chars(first, last, asInt);
                fc.ec == std::errc{} && fc.ptr == last) {
                return nlohmann::json(asInt);
            }
            // libc++ on Apple Clang 16 / Xcode 16.4 has not yet implemented
            // std::from_chars for floating-point types. Use std::strtod for
            // portability; raw is a std::string and therefore null-terminated.
            char* end = nullptr;
            errno = 0;
            const double asDouble = std::strtod(raw.c_str(), &end);
            if (errno == 0 && end == raw.c_str() + raw.size()) {
                return nlohmann::json(asDouble);
            }
        }
        return nlohmann::json(raw);
    }

    if (node.IsSequence()) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& item : node) arr.push_back(yamlToJson(item, depth + 1));
        return arr;
    }

    if (node.IsMap()) {
        nlohmann::json obj = nlohmann::json::object();
        for (const auto& kv : node) {
            obj[kv.first.as<std::string>("")] = yamlToJson(kv.second, depth + 1);
        }
        return obj;
    }

    return nlohmann::json{nullptr};
}

}  // namespace

std::expected<ImportFromOpenApi::Outcome, ChainApiError> ImportFromOpenApi::run(
    const fs::path& spec, const fs::path& projectRoot) const {
    auto canonical = canonicalSpecPath(spec, projectRoot);
    if (!canonical) return std::unexpected(canonical.error());

    // 8 MiB cap. Real-world OpenAPI specs (Stripe, GitHub) clock in around
    // 5 MiB; anything bigger is almost certainly an attack or a runaway
    // generator. yaml-cpp will happily allocate gigabytes if we let it.
    constexpr std::uintmax_t kMaxSpecBytes = std::uintmax_t{8} * 1024 * 1024;
    std::error_code sizeEc;
    const auto specSize = fs::file_size(*canonical, sizeEc);
    if (sizeEc) {
        return std::unexpected(invalid("openapi import: cannot stat spec: " + sizeEc.message()));
    }
    if (specSize > kMaxSpecBytes) {
        return std::unexpected(invalid("openapi import: spec exceeds 8 MiB cap (got " +
                                       std::to_string(specSize) + " bytes)"));
    }

    std::ifstream in(*canonical);
    if (!in) {
        return std::unexpected(invalid("openapi import: cannot open spec: " + canonical->string()));
    }
    std::stringstream buffer;
    buffer << in.rdbuf();

    YAML::Node root;
    try {
        root = YAML::Load(buffer.str());
    } catch (const YAML::Exception& e) {
        return std::unexpected(
            ChainApiError{ErrorCode::YamlParse,
                          ErrorClass::Schema,
                          std::string{"openapi import: YAML parse failed: "} + e.what()});
    }

    if (!root || !root.IsMap()) {
        return std::unexpected(
            invalid("openapi import: top-level document must be a YAML/JSON object"));
    }
    const auto openapiVersion = root["openapi"].as<std::string>("");
    if (openapiVersion.empty() || !openapiVersion.starts_with("3.")) {
        return std::unexpected(invalid("openapi import: only OpenAPI 3.x is supported (found '" +
                                       openapiVersion + "')"));
    }

    const auto& info = root["info"];
    const std::string title =
        (info && info.IsMap()) ? info["title"].as<std::string>("Imported") : "Imported";

    const auto& paths = root["paths"];
    if (!paths || !paths.IsMap() || paths.size() == 0) {
        return std::unexpected(invalid("openapi import: `paths` is missing or empty"));
    }

    Outcome outcome;
    outcome.project.name = title;
    outcome.project.defaultEnvironment = "default";
    outcome.project.environments["default"]["baseUrl"] = firstServerUrl(root);

    const auto importedAt = nowIso8601Utc();
    std::vector<std::string> warnings;

    std::set<std::string> seenOpIds;

    for (const auto& pathKv : paths) {
        const auto pathTemplate = pathKv.first.as<std::string>("");
        const auto& pathItem = pathKv.second;
        if (pathTemplate.empty() || !pathItem || !pathItem.IsMap()) continue;

        const auto derivation = deriveResourceIdDetailed(pathTemplate);
        const auto& resourceId = derivation.id;
        auto& resource = outcome.project.resources[ResourceId{resourceId}];
        if (resource.id.value.empty()) resource.id = ResourceId{resourceId};

        for (const auto& methodKv : pathItem) {
            const auto methodLower = toLowerAscii(methodKv.first.as<std::string>(""));
            if (!isOpenApiMethod(methodLower)) continue;

            const auto& opNode = methodKv.second;
            if (!opNode || !opNode.IsMap()) continue;

            auto opName = deriveOperationName(methodLower, pathTemplate);
            // Disambiguate collisions (e.g. two GETs on different paths
            // mapping to the same resource both calling themselves "list").
            std::string uniqueName = opName;
            int suffix = 2;
            while (resource.operations.contains(uniqueName) ||
                   seenOpIds.contains(resourceId + "." + uniqueName)) {
                uniqueName = opName + "_" + std::to_string(suffix++);
            }
            seenOpIds.insert(resourceId + "." + uniqueName);

            Operation op;
            op.id = OperationId{resourceId + "." + uniqueName};
            op.resource = ResourceId{resourceId};
            op.method = parseHttpMethod(methodLower);

            auto pathRewrite = rewritePathParams(pathTemplate, resourceId);
            op.pathTemplate = std::move(pathRewrite.rewritten);
            op.expectStatus = pickExpectedStatus(opNode["responses"]);

            Provenance prov;
            prov.source = Provenance::Source::OpenApiImport;
            prov.verifiedAgainst = Provenance::VerifiedAgainst::None;
            prov.importedAt = importedAt;
            if (derivation.singularised && !derivation.fromSegment.empty()) {
                prov.evidence["resource_derivation"] = "singularised path segment '" +
                                                       derivation.fromSegment + "' to '" +
                                                       resourceId + "'";
            }
            if (const auto opSummary = opNode["summary"].as<std::string>(""); !opSummary.empty()) {
                prov.evidence["summary"] = opSummary;
            }
            if (const auto opIdSpec = opNode["operationId"].as<std::string>("");
                !opIdSpec.empty()) {
                prov.evidence["operationId"] = opIdSpec;
            }
            for (const auto& param : pathRewrite.paramNames) {
                prov.evidence["path_param." + param] = "rewritten to {{" + resourceId + "." +
                                                       param +
                                                       "}}; "
                                                       "ensure an upstream operation extracts " +
                                                       param;
            }

            // Top-level scalar response fields become candidate extractions.
            // When an example payload is present, the verifier confirms
            // each candidate path; results land in provenance evidence.
            const auto schemaNode = firstJsonResponseSchema(opNode);
            const auto exampleNode = firstJsonResponseExample(opNode);
            const auto candidates = inferExtractionsFromSchema(schemaNode);
            for (const auto& [name, path] : candidates) {
                Extraction ext;
                ext.variableName = name;
                ext.sourcePath = path;
                ext.source = Extraction::Source::JsonPath;
                op.extractions.push_back(std::move(ext));
            }

            if (!op.extractions.empty() && exampleNode) {
                Verifier verifier;
                SampleResponse sample;
                sample.kind = Provenance::VerifiedAgainst::OpenApiExample;
                sample.statusCode = op.expectStatus.value_or(200);
                const auto exampleJson = yamlToJson(*exampleNode);
                sample.body = exampleJson.dump();
                if (auto report = verifier.verify(op, sample); report) {
                    bool anyVerified = false;
                    bool anyFailure = false;
                    for (const auto& v : report->extractions) {
                        std::string tag;
                        switch (v.status) {
                            case VerificationStatus::Verified:
                                tag = "verified";
                                anyVerified = true;
                                break;
                            case VerificationStatus::Null:
                                tag = "null";
                                anyFailure = true;
                                break;
                            case VerificationStatus::NoMatch:
                                tag = "no_match";
                                anyFailure = true;
                                break;
                            case VerificationStatus::NotEvaluated:
                                tag = "not_evaluated";
                                break;
                            case VerificationStatus::NoSample:
                                tag = "no_sample";
                                break;
                        }
                        prov.evidence["extract." + v.variableName] = tag + ": " + v.detail;
                    }
                    if (anyVerified) {
                        prov.verifiedAgainst = Provenance::VerifiedAgainst::OpenApiExample;
                    }
                    if (anyFailure) {
                        warnings.push_back("operation " + op.id.value +
                                           ": one or more inferred extractions did not "
                                           "match the response example — review before "
                                           "running");
                    }
                } else {
                    warnings.push_back("operation " + op.id.value + ": verifier rejected sample (" +
                                       report.error().detail + ")");
                }
            } else if (!op.extractions.empty()) {
                warnings.push_back("operation " + op.id.value + ": " +
                                   std::to_string(op.extractions.size()) +
                                   " extraction(s) inferred from schema but no response "
                                   "example was available to verify them");
            }

            op.provenance = std::move(prov);

            // Each rewritten path parameter shows up as one warning so the
            // user's review loop sees exactly what needs to be linked.
            for (const auto& param : pathRewrite.paramNames) {
                warnings.push_back("operation " + op.id.value + ": path parameter `" + param +
                                   "` resolves to {{" + resourceId + "." + param +
                                   "}} — wire an upstream operation that extracts `" + param +
                                   "` into resource `" + resourceId + "`");
            }

            resource.operations.emplace(uniqueName, std::move(op));
        }
    }

    if (outcome.project.resources.empty()) {
        return std::unexpected(invalid("openapi import: spec yielded zero importable operations"));
    }

    std::ostringstream wbuf;
    for (const auto& w : warnings) wbuf << w << '\n';
    outcome.warnings = wbuf.str();
    return outcome;
}

}  // namespace chainapi::engine
