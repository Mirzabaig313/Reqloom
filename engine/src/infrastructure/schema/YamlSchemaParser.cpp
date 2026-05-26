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
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
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
        if (kv.second.IsScalar()) {
            result[key] = kv.second.as<std::string>();
        } else {
            YAML::Emitter emitter;
            emitter << kv.second;
            result[key] = emitter.c_str();
        }
    }
    return result;
}

nlohmann::json yamlNodeToJsonValue(const YAML::Node& node);

nlohmann::json yamlScalarToJsonValue(const YAML::Node& scalar) {
    const auto raw = scalar.as<std::string>();
    // Use parens, not braces — nlohmann::json{x} treats braces as an
    // initializer-list and produces a one-element array.
    if (raw == "true") return nlohmann::json(true);
    if (raw == "false") return nlohmann::json(false);
    if (raw == "null" || raw == "~") return nlohmann::json(nullptr);
    if (!raw.empty() && (std::isdigit(static_cast<unsigned char>(raw.front())) ||
                         raw.front() == '-' || raw.front() == '+')) {
        long long ll = 0;
        const auto* first = raw.data();
        const auto* last = first + raw.size();
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
    if (node.IsScalar()) return yamlScalarToJsonValue(node);
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
        if (ext.sourcePath.starts_with("$.headers.")) {
            ext.source = Extraction::Source::Header;
        }
        result.push_back(std::move(ext));
    }
    return result;
}

std::chrono::seconds parseDuration(const std::string& s) {
    if (s.empty()) return std::chrono::seconds{900};  // default 15m
    auto value = std::stol(s.substr(0, s.size() - 1));
    char unit = s.back();
    switch (unit) {
        case 's':
            return std::chrono::seconds{value};
        case 'm':
            return std::chrono::seconds{value * 60};
        case 'h':
            return std::chrono::seconds{value * 3600};
        case 'd':
            return std::chrono::seconds{value * 86400};
        default:
            return std::chrono::seconds{value};
    }
}

/// Resolve a hook-script value: if it looks like a relative path to a
/// `.js`/`.mjs` file, load the file content; otherwise treat the value
/// as inline JS.
///
/// Heuristic for "is a path":
///   - starts with "./" or "../"
///   - OR ends with ".js" / ".mjs" with no whitespace, `=`, `{`, or `(`
///
/// Security:
///   - Path is canonicalised via `weakly_canonical` against `baseDir`.
///     Resolved paths outside the root are rejected.
///   - File size is capped at 1 MiB.
[[nodiscard]] std::expected<std::string, ChainApiError> resolveHookScript(const std::string& value,
                                                                          const fs::path& baseDir) {
    if (value.empty()) return value;

    const auto looksLikeRelativePath = [](std::string_view s) {
        if (s.starts_with("./") || s.starts_with("../")) return true;
        if (s.find('\n') != std::string_view::npos) return false;
        if (s.find('{') != std::string_view::npos) return false;
        if (s.find('(') != std::string_view::npos) return false;
        if (s.find('=') != std::string_view::npos) return false;
        return s.ends_with(".js") || s.ends_with(".mjs");
    };
    if (!looksLikeRelativePath(value)) return value;

    const fs::path raw{value};
    if (raw.is_absolute()) {
        return std::unexpected(
            ChainApiError{ErrorCode::SchemaInvalid,
                          ErrorClass::Schema,
                          "hook script path must be relative to the project root: " + value});
    }

    std::error_code ec;
    const auto canonical = fs::weakly_canonical(baseDir / raw, ec);
    if (ec) {
        return std::unexpected(ChainApiError{
            ErrorCode::SchemaInvalid,
            ErrorClass::Schema,
            "hook script path is not resolvable: " + value + " (" + ec.message() + ")"});
    }

    // Containment check using fs::canonical (not weakly_canonical) for the
    // base — fully resolving symlinks is required for the prefix comparison
    // to be reliable. Also require the prefix to end at a path separator to
    // prevent /home/user/proj matching /home/user/proj-evil/hook.js.
    const auto canonicalBase = fs::canonical(baseDir, ec);
    if (ec) {
        return std::unexpected(
            ChainApiError{ErrorCode::SchemaInvalid,
                          ErrorClass::Schema,
                          "could not canonicalise project root: " + ec.message()});
    }
    {
        const auto canonStr = canonical.lexically_normal().string();
        const auto baseStr = canonicalBase.lexically_normal().string();
        const bool contained =
            canonStr.size() >= baseStr.size() && canonStr.substr(0, baseStr.size()) == baseStr &&
            (canonStr.size() == baseStr.size() || canonStr[baseStr.size()] == '/' ||
             canonStr[baseStr.size()] == fs::path::preferred_separator);
        if (!contained) {
            return std::unexpected(
                ChainApiError{ErrorCode::SchemaInvalid,
                              ErrorClass::Schema,
                              "hook script path escapes the project root: " + value});
        }
    }

    if (!fs::exists(canonical, ec) || ec) {
        return std::unexpected(ChainApiError{ErrorCode::SchemaInvalid,
                                             ErrorClass::Schema,
                                             "hook script not found: " + canonical.string()});
    }
    if (!fs::is_regular_file(canonical, ec) || ec) {
        return std::unexpected(
            ChainApiError{ErrorCode::SchemaInvalid,
                          ErrorClass::Schema,
                          "hook script is not a regular file: " + canonical.string()});
    }

    constexpr std::uintmax_t kMaxHookBytes = 1 * 1024 * 1024;  // 1 MiB
    const auto size = fs::file_size(canonical, ec);
    if (ec) {
        return std::unexpected(ChainApiError{
            ErrorCode::SchemaInvalid,
            ErrorClass::Schema,
            "could not stat hook script " + canonical.string() + ": " + ec.message()});
    }
    if (size > kMaxHookBytes) {
        return std::unexpected(
            ChainApiError{ErrorCode::SchemaInvalid,
                          ErrorClass::Schema,
                          "hook script exceeds 1 MiB cap: " + canonical.string()});
    }

    std::ifstream in(canonical, std::ios::binary);
    if (!in) {
        return std::unexpected(
            ChainApiError{ErrorCode::SchemaInvalid,
                          ErrorClass::Schema,
                          "hook script could not be opened: " + canonical.string()});
    }
    std::string contents(static_cast<std::size_t>(size), '\0');
    in.read(contents.data(), static_cast<std::streamsize>(size));
    if (in.gcount() != static_cast<std::streamsize>(size)) {
        return std::unexpected(ChainApiError{ErrorCode::SchemaInvalid,
                                             ErrorClass::Schema,
                                             "hook script read truncated: " + canonical.string()});
    }
    return contents;
}

// ─── Actor parsing ───────────────────────────────────────────────────────────

Actor parseActor(const std::string& actorId, const YAML::Node& node) {
    Actor actor;
    actor.id = ActorId{actorId};
    actor.description = node["description"].as<std::string>("");

    const auto& auth = node["auth"];
    if (auth) {
        const auto strategy = auth["strategy"].as<std::string>("simple");
        if (strategy == "chain") {
            actor.strategy = AuthStrategy::Chain;
        } else if (strategy == "basic") {
            actor.strategy = AuthStrategy::Basic;
        } else if (strategy == "api_key") {
            actor.strategy = AuthStrategy::ApiKey;
        } else if (strategy == "oauth2_client_credentials") {
            actor.strategy = AuthStrategy::OAuth2ClientCredentials;
        } else if (strategy == "oauth2_password") {
            actor.strategy = AuthStrategy::OAuth2Password;
        } else if (strategy == "oauth1") {
            actor.strategy = AuthStrategy::OAuth1;
        } else if (strategy == "aws_sigv4") {
            actor.strategy = AuthStrategy::AwsSigV4;
        } else {
            actor.strategy = AuthStrategy::Simple;
        }

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
        } else if (actor.strategy == AuthStrategy::Basic) {
            actor.authConfig["username"] = auth["username"].as<std::string>("");
            actor.authConfig["password"] = auth["password"].as<std::string>("");
        } else if (actor.strategy == AuthStrategy::ApiKey) {
            // Required: `key`. Optional: `location` (header|query|cookie) and `name`.
            actor.authConfig["key"] = auth["key"].as<std::string>("");
            if (auth["location"]) {
                actor.authConfig["location"] = auth["location"].as<std::string>();
            }
            if (auth["name"]) {
                actor.authConfig["name"] = auth["name"].as<std::string>();
            }
        } else if (actor.strategy == AuthStrategy::OAuth2ClientCredentials) {
            // RFC 6749 §4.4: token_url + client_id + client_secret required; scope optional.
            actor.authConfig["token_url"] = auth["token_url"].as<std::string>("");
            actor.authConfig["client_id"] = auth["client_id"].as<std::string>("");
            actor.authConfig["client_secret"] = auth["client_secret"].as<std::string>("");
            if (auth["scope"]) {
                actor.authConfig["scope"] = auth["scope"].as<std::string>();
            }
        } else if (actor.strategy == AuthStrategy::OAuth2Password) {
            // RFC 6749 §4.3: same as client_credentials plus username/password.
            actor.authConfig["token_url"] = auth["token_url"].as<std::string>("");
            actor.authConfig["client_id"] = auth["client_id"].as<std::string>("");
            actor.authConfig["client_secret"] = auth["client_secret"].as<std::string>("");
            actor.authConfig["username"] = auth["username"].as<std::string>("");
            actor.authConfig["password"] = auth["password"].as<std::string>("");
            if (auth["scope"]) {
                actor.authConfig["scope"] = auth["scope"].as<std::string>();
            }
        } else if (actor.strategy == AuthStrategy::OAuth1) {
            // RFC 5849 two-legged + optional preacquired access token.
            actor.authConfig["consumer_key"] = auth["consumer_key"].as<std::string>("");
            actor.authConfig["consumer_secret"] = auth["consumer_secret"].as<std::string>("");
            if (auth["token"]) actor.authConfig["token"] = auth["token"].as<std::string>();
            if (auth["token_secret"])
                actor.authConfig["token_secret"] = auth["token_secret"].as<std::string>();
            if (auth["realm"]) actor.authConfig["realm"] = auth["realm"].as<std::string>();
        } else if (actor.strategy == AuthStrategy::AwsSigV4) {
            // AWS SigV4 (AWS4-HMAC-SHA256). Long-lived keys are discouraged;
            // prefer {{X.y}} references that pull from a secret store.
            actor.authConfig["access_key"] = auth["access_key"].as<std::string>("");
            actor.authConfig["secret_key"] = auth["secret_key"].as<std::string>("");
            actor.authConfig["region"] = auth["region"].as<std::string>("");
            actor.authConfig["service"] = auth["service"].as<std::string>("");
            if (auth["session_token"]) {
                actor.authConfig["session_token"] = auth["session_token"].as<std::string>();
            }
            if (auth["sign_payload"]) {
                actor.authConfig["sign_payload"] =
                    auth["sign_payload"].as<bool>() ? "true" : "false";
            }
        } else {
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

    if (node["inject"]) {
        actor.inject.headers = parseStringMap(node["inject"]["headers"]);
    }

    return actor;
}

// ─── Resource/Operation parsing ──────────────────────────────────────────────

std::expected<Resource, ChainApiError> parseResource(const std::string& resourceId,
                                                     const YAML::Node& node,
                                                     const fs::path& baseDir) {
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
            // expect_status accepts either a scalar or an array. The array
            // form is required when poll_until is in play (202 Accepted
            // alongside a 200 from the eventual poll completion).
            const auto& es = opNode["expect_status"];
            if (es.IsSequence()) {
                for (const auto& s : es) {
                    op.expectStatusList.push_back(s.as<int>());
                }
            } else if (es.IsScalar()) {
                op.expectStatus = es.as<int>();
            }
        }

        if (opNode["poll_until"]) {
            const auto& p = opNode["poll_until"];
            PollUntil poll;
            poll.method = parseMethod(p["method"].as<std::string>("GET"));
            poll.pathTemplate = p["path"].as<std::string>("");
            if (p["actor"]) {
                poll.actor = ActorId{p["actor"].as<std::string>()};
            }
            poll.successWhen = p["success_when"].as<std::string>("");
            if (p["fail_when"]) {
                poll.failWhen = p["fail_when"].as<std::string>();
            }
            if (p["interval"]) {
                // parseDuration returns seconds; read milliseconds directly
                // when the literal ends in 'ms'.
                const auto literal = p["interval"].as<std::string>("2s");
                if (literal.ends_with("ms")) {
                    poll.interval =
                        std::chrono::milliseconds{std::stol(literal.substr(0, literal.size() - 2))};
                } else {
                    poll.interval = std::chrono::duration_cast<std::chrono::milliseconds>(
                        parseDuration(literal));
                }
            }
            if (p["backoff"]) {
                const auto& b = p["backoff"];
                if (b["base"]) {
                    const auto literal = b["base"].as<std::string>("500ms");
                    if (literal.ends_with("ms")) {
                        poll.backoffBase = std::chrono::milliseconds{
                            std::stol(literal.substr(0, literal.size() - 2))};
                    } else {
                        poll.backoffBase = std::chrono::duration_cast<std::chrono::milliseconds>(
                            parseDuration(literal));
                    }
                }
                if (b["max"]) {
                    const auto literal = b["max"].as<std::string>("30s");
                    if (literal.ends_with("ms")) {
                        poll.backoffMax = std::chrono::milliseconds{
                            std::stol(literal.substr(0, literal.size() - 2))};
                    } else {
                        poll.backoffMax = std::chrono::duration_cast<std::chrono::milliseconds>(
                            parseDuration(literal));
                    }
                }
            }
            if (p["timeout"]) {
                const auto literal = p["timeout"].as<std::string>("60s");
                if (literal.ends_with("ms")) {
                    poll.timeout =
                        std::chrono::milliseconds{std::stol(literal.substr(0, literal.size() - 2))};
                } else {
                    poll.timeout = std::chrono::duration_cast<std::chrono::milliseconds>(
                        parseDuration(literal));
                }
            }
            if (p["max_attempts"]) {
                poll.maxAttempts = p["max_attempts"].as<int>();
            }
            op.pollUntil = std::move(poll);
        }

        op.extractions = parseExtractions(opNode["extract"]);

        if (opNode["depends_on"] && opNode["depends_on"].IsSequence()) {
            for (const auto& dep : opNode["depends_on"]) {
                op.explicitDependencies.push_back(OperationId{dep.as<std::string>()});
            }
        }

        // Hook scripts: a relative `.js` path is loaded from disk; everything
        // else is treated as inline JS. See `resolveHookScript` for the
        // heuristic and security checks.
        if (opNode["pre_request"]) {
            auto resolved = resolveHookScript(opNode["pre_request"].as<std::string>(), baseDir);
            if (!resolved) return std::unexpected(resolved.error());
            op.preRequestScript = std::move(*resolved);
        }
        if (opNode["post_response"]) {
            auto resolved = resolveHookScript(opNode["post_response"].as<std::string>(), baseDir);
            if (!resolved) return std::unexpected(resolved.error());
            op.postResponseScript = std::move(*resolved);
        }

        if (opNode["retry"]) {
            const auto& retryNode = opNode["retry"];
            if (retryNode["max"]) op.retry.maxAttempts = retryNode["max"].as<int>();
            if (retryNode["backoff"]) {
                op.retry.baseBackoff = std::chrono::milliseconds{retryNode["backoff"].as<int>(500)};
            }
        }

        if (opNode["timeout"]) {
            op.timeout = std::chrono::milliseconds{opNode["timeout"].as<int>(30000)};
        }

        op.force = opNode["force"].as<bool>(false);

        resource.operations[opName] = std::move(op);
    }

    return resource;
}

// ─── File loading ────────────────────────────────────────────────────────────

std::vector<fs::path> resolveGlob(const fs::path& baseDir, const std::string& pattern) {
    std::vector<fs::path> results;
    auto dir = baseDir / fs::path(pattern).parent_path();
    auto ext = fs::path(pattern).filename().string();

    if (!fs::exists(dir)) return results;

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
            ErrorCode::YamlParse, ErrorClass::Schema, "File not found: " + rootYaml.string()});
    }

    YAML::Node root;
    try {
        root = YAML::LoadFile(rootYaml.string());
    } catch (const YAML::Exception& e) {
        return std::unexpected(ChainApiError{
            ErrorCode::YamlParse, ErrorClass::Schema, rootYaml.string() + ": " + e.what()});
    }

    auto version = root["version"].as<int>(0);
    if (version < 1 || version > 3) {
        return std::unexpected(
            ChainApiError{ErrorCode::SchemaVersion,
                          ErrorClass::Schema,
                          "Unsupported schema version " + std::to_string(version) +
                              " (supported: 1–3). Run `chainapi migrate` to upgrade."});
    }

    Project project;
    project.name = root["name"].as<std::string>("Unnamed Project");
    project.defaultEnvironment = root["default_environment"].as<std::string>("local");

    const auto baseDir = rootYaml.parent_path();

    auto loadSubFile = [&](const fs::path& file) -> std::optional<ChainApiError> {
        YAML::Node subDoc;
        try {
            subDoc = YAML::LoadFile(file.string());
        } catch (const YAML::Exception& e) {
            return ChainApiError{
                ErrorCode::YamlParse, ErrorClass::Schema, file.string() + ": " + e.what()};
        }

        const auto relPath = fs::relative(file, baseDir).string();
        if (relPath.starts_with("actors/") || relPath.starts_with("actors\\")) {
            // Two file shapes accepted:
            //   Form A (flat):    name: vendor\n description: ...\n auth: ...
            //   Form B (wrapped): vendor:\n   description: ...\n   auth: ...
            auto actorId = subDoc["name"].as<std::string>("");
            const YAML::Node actorBody = subDoc["name"] ? subDoc : [&]() {
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
            auto parsedResource = parseResource(resourceId, resourceBody, baseDir);
            if (!parsedResource) return parsedResource.error();
            project.resources[ResourceId{resourceId}] = std::move(*parsedResource);
        } else if (relPath.starts_with("environments/") || relPath.starts_with("environments\\")) {
            // Two env file shapes accepted:
            //   Form A (wrapped): name: local\n variables:\n   baseUrl: ...
            //   Form B (flat):    baseUrl: ...\n admin_email: ...
            auto envName = subDoc["name"].as<std::string>(file.stem().string());
            std::map<std::string, std::string> vars;
            if (subDoc["variables"] && subDoc["variables"].IsMap()) {
                for (const auto& kv : subDoc["variables"]) {
                    vars[kv.first.as<std::string>()] = kv.second.as<std::string>("");
                }
            } else if (subDoc.IsMap()) {
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

    auto processImports = [&](const YAML::Node& importsNode) -> std::optional<ChainApiError> {
        if (!importsNode) return std::nullopt;

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

    if (root["actors"] && root["actors"].IsMap()) {
        for (const auto& kv : root["actors"]) {
            auto actorId = kv.first.as<std::string>();
            project.actors[ActorId{actorId}] = parseActor(actorId, kv.second);
        }
    }

    if (root["resources"] && root["resources"].IsMap()) {
        for (const auto& kv : root["resources"]) {
            auto resourceId = kv.first.as<std::string>();
            auto parsedResource = parseResource(resourceId, kv.second, baseDir);
            if (!parsedResource) {
                return std::unexpected(parsedResource.error());
            }
            project.resources[ResourceId{resourceId}] = std::move(*parsedResource);
        }
    }

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
