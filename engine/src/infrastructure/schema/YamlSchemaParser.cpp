// YamlSchemaParser — yaml-cpp-backed schema parser.
//
// Parses chainapi.yaml + imported sub-files into a validated Project.
// Validates schema version, detects cycles, and resolves undefined refs.
#include "YamlSchemaParser.h"

#include <yaml-cpp/yaml.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace chainapi::engine {

namespace {

// ─── Helpers ─────────────────────────────────────────────────────────────────

HttpMethod parseMethod(const std::string& m) {
    if (m == "GET" || m == "get") return HttpMethod::Get;
    if (m == "POST" || m == "post") return HttpMethod::Post;
    if (m == "PUT" || m == "put") return HttpMethod::Put;
    if (m == "PATCH" || m == "patch") return HttpMethod::Patch;
    if (m == "DELETE" || m == "delete") return HttpMethod::Delete;
    if (m == "HEAD" || m == "head") return HttpMethod::Head;
    if (m == "OPTIONS" || m == "options") return HttpMethod::Options;
    return HttpMethod::Get;
}

std::map<std::string, std::string> parseStringMap(const YAML::Node& node) {
    std::map<std::string, std::string> result;
    if (!node || !node.IsMap()) return result;
    for (const auto& kv : node) {
        auto key = kv.first.as<std::string>();
        // Values can be strings, numbers, or booleans — stringify them all.
        if (kv.second.IsScalar()) {
            result[key] = kv.second.as<std::string>();
        } else {
            // For complex values (arrays/maps in body), dump as JSON-ish string.
            // The variable resolver will re-parse if needed.
            YAML::Emitter emitter;
            emitter << kv.second;
            result[key] = emitter.c_str();
        }
    }
    return result;
}

// Convert a yaml-cpp Node to a JSON string suitable as an HTTP request body.
// Quotes strings, recurses into maps/sequences. Numbers and booleans are
// emitted as JSON literals (yaml-cpp gives us strings only, so we sniff).
nlohmann::json yamlNodeToJsonValue(const YAML::Node& node);

nlohmann::json yamlScalarToJsonValue(const YAML::Node& scalar) {
    const auto raw = scalar.as<std::string>();
    // NOTE: use direct constructor (parens), NOT brace-init, because
    // nlohmann::json{x} treats braces as an array initializer-list, so
    // `json{"foo"}` makes ["foo"], not "foo".
    if (raw == "true")  return nlohmann::json(true);
    if (raw == "false") return nlohmann::json(false);
    if (raw == "null" || raw == "~") return nlohmann::json(nullptr);
    if (!raw.empty() && (std::isdigit(static_cast<unsigned char>(raw.front()))
                          || raw.front() == '-' || raw.front() == '+')) {
        long long ll = 0;
        const auto* first = raw.data();
        const auto* last  = first + raw.size();
        auto fc = std::from_chars(first, last, ll);
        if (fc.ec == std::errc{} && fc.ptr == last) {
            return nlohmann::json(ll);
        }
        try {
            std::size_t pos = 0;
            double d = std::stod(raw, &pos);
            if (pos == raw.size()) return nlohmann::json(d);
        } catch (...) {
            // fall through to string
        }
    }
    return nlohmann::json(raw);
}

nlohmann::json yamlNodeToJsonValue(const YAML::Node& node) {
    if (!node || node.IsNull()) return nlohmann::json(nullptr);
    if (node.IsScalar())        return yamlScalarToJsonValue(node);
    if (node.IsSequence()) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& item : node) {
            arr.push_back(yamlNodeToJsonValue(item));
        }
        return arr;
    }
    if (node.IsMap()) {
        nlohmann::json obj = nlohmann::json::object();
        for (const auto& kv : node) {
            obj[kv.first.as<std::string>()] = yamlNodeToJsonValue(kv.second);
        }
        return obj;
    }
    return nlohmann::json(nullptr);
}

std::string nodeToJsonString(const YAML::Node& node) {
    if (!node || node.IsNull()) return "";
    return yamlNodeToJsonValue(node).dump();
}

std::vector<Extraction> parseExtractions(const YAML::Node& node) {
    std::vector<Extraction> result;
    if (!node || !node.IsMap()) return result;
    for (const auto& kv : node) {
        Extraction ext;
        ext.variableName = kv.first.as<std::string>();
        ext.sourcePath = kv.second.as<std::string>();
        // Default to JsonPath; if it starts with "$.headers." treat as Header.
        if (ext.sourcePath.starts_with("$.headers.")) {
            ext.source = Extraction::Source::Header;
        }
        result.push_back(std::move(ext));
    }
    return result;
}

std::chrono::seconds parseDuration(const std::string& s) {
    // Supports: "15m", "1h", "24h", "30s", "7d"
    if (s.empty()) return std::chrono::seconds{900};  // default 15m

    auto value = std::stol(s.substr(0, s.size() - 1));
    char unit = s.back();
    switch (unit) {
        case 's': return std::chrono::seconds{value};
        case 'm': return std::chrono::seconds{value * 60};
        case 'h': return std::chrono::seconds{value * 3600};
        case 'd': return std::chrono::seconds{value * 86400};
        default: return std::chrono::seconds{value};  // assume seconds
    }
}

// ─── Actor parsing ───────────────────────────────────────────────────────────

Actor parseActor(const std::string& actorId, const YAML::Node& node) {
    Actor actor;
    actor.id = ActorId{actorId};
    actor.description = node["description"].as<std::string>("");

    const auto& auth = node["auth"];
    if (auth) {
        auto strategy = auth["strategy"].as<std::string>("simple");
        actor.strategy = (strategy == "chain") ? AuthStrategy::Chain : AuthStrategy::Simple;

        if (actor.strategy == AuthStrategy::Chain && auth["steps"]) {
            for (const auto& stepNode : auth["steps"]) {
                AuthStep step;
                step.id = stepNode["id"].as<std::string>("");
                step.method = parseMethod(stepNode["method"].as<std::string>("POST"));
                step.pathTemplate = stepNode["path"].as<std::string>("");
                step.headers = parseStringMap(stepNode["headers"]);
                if (stepNode["body"]) {
                    step.bodyTemplate = nodeToJsonString(stepNode["body"]);
                }
                if (stepNode["expect_status"]) {
                    step.expectStatus = stepNode["expect_status"].as<int>();
                }
                step.extractions = parseExtractions(stepNode["extract"]);
                actor.authSteps.push_back(std::move(step));
            }
        } else {
            // Simple strategy — single auth request
            AuthStep step;
            step.id = "login";
            step.method = parseMethod(auth["method"].as<std::string>("POST"));
            step.pathTemplate = auth["path"].as<std::string>("");
            step.headers = parseStringMap(auth["headers"]);
            if (auth["body"]) {
                step.bodyTemplate = nodeToJsonString(auth["body"]);
            }
            if (auth["expect_status"]) {
                step.expectStatus = auth["expect_status"].as<int>();
            }
            step.extractions = parseExtractions(auth["extract"]);
            actor.authSteps.push_back(std::move(step));
        }
    }

    // Session
    if (node["session"]) {
        const auto& session = node["session"];
        if (session["ttl"]) {
            actor.sessionTtl = parseDuration(session["ttl"].as<std::string>("15m"));
        }
        if (session["refresh"]) {
            SessionRefresh refresh;
            const auto& r = session["refresh"];
            refresh.method = parseMethod(r["method"].as<std::string>("POST"));
            refresh.pathTemplate = r["path"].as<std::string>("");
            refresh.headers = parseStringMap(r["headers"]);
            if (r["body"]) {
                refresh.bodyTemplate = nodeToJsonString(r["body"]);
            }
            refresh.extractions = parseExtractions(r["extract"]);
            actor.refresh = std::move(refresh);
        }
    }

    // Inject
    if (node["inject"]) {
        actor.inject.headers = parseStringMap(node["inject"]["headers"]);
    }

    return actor;
}

// ─── Resource/Operation parsing ──────────────────────────────────────────────

Resource parseResource(const std::string& resourceId, const YAML::Node& node) {
    Resource resource;
    resource.id = ResourceId{resourceId};
    resource.description = node["description"].as<std::string>("");

    const auto& ops = node["operations"];
    if (!ops || !ops.IsMap()) return resource;

    for (const auto& kv : ops) {
        auto opName = kv.first.as<std::string>();
        const auto& opNode = kv.second;

        Operation op;
        op.id = OperationId{resourceId + "." + opName};
        op.resource = ResourceId{resourceId};
        op.method = parseMethod(opNode["method"].as<std::string>("GET"));
        op.pathTemplate = opNode["path"].as<std::string>("");

        if (opNode["actor"]) {
            op.actor = ActorId{opNode["actor"].as<std::string>()};
        }

        op.headers = parseStringMap(opNode["headers"]);
        op.queryParams = parseStringMap(opNode["query_params"]);

        if (opNode["body"]) {
            op.bodyTemplate = nodeToJsonString(opNode["body"]);
        }
        if (opNode["body_form"]) {
            op.bodyForm = parseStringMap(opNode["body_form"]);
        }

        if (opNode["expect_status"]) {
            op.expectStatus = opNode["expect_status"].as<int>();
        }

        op.extractions = parseExtractions(opNode["extract"]);

        // Explicit depends_on
        if (opNode["depends_on"] && opNode["depends_on"].IsSequence()) {
            for (const auto& dep : opNode["depends_on"]) {
                op.explicitDependencies.push_back(OperationId{dep.as<std::string>()});
            }
        }

        // Hooks
        if (opNode["pre_request"]) {
            op.preRequestScript = opNode["pre_request"].as<std::string>();
        }
        if (opNode["post_response"]) {
            op.postResponseScript = opNode["post_response"].as<std::string>();
        }

        // Retry
        if (opNode["retry"]) {
            const auto& retryNode = opNode["retry"];
            if (retryNode["max"]) op.retry.maxAttempts = retryNode["max"].as<int>();
            if (retryNode["backoff"]) {
                op.retry.baseBackoff = std::chrono::milliseconds{
                    retryNode["backoff"].as<int>(500)};
            }
        }

        // Timeout
        if (opNode["timeout"]) {
            op.timeout = std::chrono::milliseconds{opNode["timeout"].as<int>(30000)};
        }

        // Force
        op.force = opNode["force"].as<bool>(false);

        resource.operations[opName] = std::move(op);
    }

    return resource;
}

// ─── File loading ────────────────────────────────────────────────────────────

std::vector<fs::path> resolveGlob(const fs::path& baseDir, const std::string& pattern) {
    // Simple glob: supports "actors/*.yaml" and "resources/*.yaml"
    std::vector<fs::path> results;
    auto dir = baseDir / fs::path(pattern).parent_path();
    auto ext = fs::path(pattern).filename().string();

    if (!fs::exists(dir)) return results;

    // If pattern ends with *.yaml, match all .yaml in that dir
    if (ext == "*.yaml" || ext == "*.yml") {
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (entry.is_regular_file()) {
                auto entryExt = entry.path().extension().string();
                if (entryExt == ".yaml" || entryExt == ".yml") {
                    results.push_back(entry.path());
                }
            }
        }
        std::sort(results.begin(), results.end());
    } else {
        // Exact file reference
        auto resolved = baseDir / pattern;
        if (fs::exists(resolved)) {
            results.push_back(resolved);
        }
    }
    return results;
}

}  // namespace

// ─── Public ──────────────────────────────────────────────────────────────────

YamlSchemaParser::YamlSchemaParser() = default;
YamlSchemaParser::~YamlSchemaParser() = default;

SchemaParseResult YamlSchemaParser::parse(const fs::path& rootYaml) {
    if (!fs::exists(rootYaml)) {
        return std::unexpected(ChainApiError{
            ErrorCode::YamlParse,
            ErrorClass::Schema,
            "File not found: " + rootYaml.string()});
    }

    YAML::Node root;
    try {
        root = YAML::LoadFile(rootYaml.string());
    } catch (const YAML::Exception& e) {
        return std::unexpected(ChainApiError{
            ErrorCode::YamlParse,
            ErrorClass::Schema,
            rootYaml.string() + ": " + e.what()});
    }

    // Version check
    auto version = root["version"].as<int>(0);
    if (version < 1 || version > 3) {
        return std::unexpected(ChainApiError{
            ErrorCode::SchemaVersion,
            ErrorClass::Schema,
            "Unsupported schema version " + std::to_string(version) +
            " (supported: 1–3). Run `chainapi migrate` to upgrade."});
    }

    Project project;
    project.name = root["name"].as<std::string>("Unnamed Project");
    project.defaultEnvironment = root["default_environment"].as<std::string>("local");

    const auto baseDir = rootYaml.parent_path();

    // Helper: load and dispatch a single sub-file based on its location.
    auto loadSubFile = [&](const fs::path& file)
        -> std::optional<ChainApiError> {
        YAML::Node subDoc;
        try {
            subDoc = YAML::LoadFile(file.string());
        } catch (const YAML::Exception& e) {
            return ChainApiError{
                ErrorCode::YamlParse,
                ErrorClass::Schema,
                file.string() + ": " + e.what()};
        }

        const auto relPath = fs::relative(file, baseDir).string();
        if (relPath.starts_with("actors/") || relPath.starts_with("actors\\")) {
            // Two file shapes accepted:
            //   Form A (flat):    name: vendor\n description: ...\n auth: ...
            //   Form B (wrapped): vendor:\n   description: ...\n   auth: ...
            auto actorId = subDoc["name"].as<std::string>("");
            const YAML::Node actorBody = subDoc["name"] ? subDoc : [&]() {
                // Wrapped form: take the first (and only) top-level key as the id.
                if (subDoc.IsMap() && subDoc.size() == 1) {
                    auto it = subDoc.begin();
                    actorId = it->first.as<std::string>();
                    return it->second;
                }
                return subDoc;
            }();
            if (actorId.empty()) actorId = file.stem().string();
            project.actors[ActorId{actorId}] = parseActor(actorId, actorBody);
        } else if (relPath.starts_with("resources/") || relPath.starts_with("resources\\")) {
            auto resourceId = subDoc["name"].as<std::string>("");
            const YAML::Node resourceBody = subDoc["name"] ? subDoc : [&]() {
                if (subDoc.IsMap() && subDoc.size() == 1) {
                    auto it = subDoc.begin();
                    resourceId = it->first.as<std::string>();
                    return it->second;
                }
                return subDoc;
            }();
            if (resourceId.empty()) resourceId = file.stem().string();
            project.resources[ResourceId{resourceId}] = parseResource(resourceId, resourceBody);
        } else if (relPath.starts_with("environments/") || relPath.starts_with("environments\\")) {
            // Two env file shapes accepted:
            //   Form A (wrapped): name: local\n variables:\n   baseUrl: ...
            //   Form B (flat):    baseUrl: ...\n admin_email: ...
            auto envName = subDoc["name"].as<std::string>(file.stem().string());
            std::map<std::string, std::string> vars;
            if (subDoc["variables"] && subDoc["variables"].IsMap()) {
                // Form A
                for (const auto& kv : subDoc["variables"]) {
                    vars[kv.first.as<std::string>()] = kv.second.as<std::string>("");
                }
            } else if (subDoc.IsMap()) {
                // Form B — every top-level scalar key (except "name") is a variable.
                for (const auto& kv : subDoc) {
                    auto key = kv.first.as<std::string>();
                    if (key == "name") continue;
                    if (kv.second.IsScalar()) {
                        vars[key] = kv.second.as<std::string>("");
                    }
                }
            }
            project.environments[envName] = std::move(vars);
        }
        return std::nullopt;
    };

    // Helper: process a YAML node that's either a single string pattern,
    // a sequence of string patterns, or a map of category → patterns.
    auto processImports = [&](const YAML::Node& importsNode)
        -> std::optional<ChainApiError> {
        if (!importsNode) return std::nullopt;

        // Form 1: flat sequence — `imports: [actors/*.yaml, resources/*.yaml]`
        // Form 2: categorised map — `imports: { actors: [...], resources: [...] }`
        // Both are valid; iterate either way and reuse the same loadSubFile logic.
        std::vector<std::string> patterns;
        if (importsNode.IsSequence()) {
            for (const auto& p : importsNode) {
                patterns.push_back(p.as<std::string>());
            }
        } else if (importsNode.IsMap()) {
            for (const auto& kv : importsNode) {
                if (kv.second.IsSequence()) {
                    for (const auto& p : kv.second) {
                        patterns.push_back(p.as<std::string>());
                    }
                } else if (kv.second.IsScalar()) {
                    patterns.push_back(kv.second.as<std::string>());
                }
            }
        }

        for (const auto& pattern : patterns) {
            auto files = resolveGlob(baseDir, pattern);
            for (const auto& file : files) {
                if (auto err = loadSubFile(file)) {
                    return err;
                }
            }
        }
        return std::nullopt;
    };

    if (auto err = processImports(root["imports"])) {
        return std::unexpected(*err);
    }

    // Inline actors (single-file format)
    if (root["actors"] && root["actors"].IsMap()) {
        for (const auto& kv : root["actors"]) {
            auto actorId = kv.first.as<std::string>();
            project.actors[ActorId{actorId}] = parseActor(actorId, kv.second);
        }
    }

    // Inline resources (single-file format)
    if (root["resources"] && root["resources"].IsMap()) {
        for (const auto& kv : root["resources"]) {
            auto resourceId = kv.first.as<std::string>();
            project.resources[ResourceId{resourceId}] = parseResource(resourceId, kv.second);
        }
    }

    // Inline environment
    if (root["environment"] && root["environment"].IsMap()) {
        std::map<std::string, std::string> vars;
        for (const auto& kv : root["environment"]) {
            vars[kv.first.as<std::string>()] = kv.second.as<std::string>("");
        }
        project.environments[project.defaultEnvironment] = std::move(vars);
    }

    return project;
}

}  // namespace chainapi::engine
