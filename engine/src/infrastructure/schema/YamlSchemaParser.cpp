// YamlSchemaParser — yaml-cpp-backed schema parser.
#include "YamlSchemaParser.h"

#include "../../domain/DependencyResolver.h"
#include "../../domain/JsonBodyTemplate.h"

#include <reqloom/engine/ProjectIdentity.h>

#include <yaml-cpp/yaml.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace reqloom::engine {

namespace {

/// A schema mistake at a YAML position, thrown inside one file's parse and turned into a
/// `ReqloomError` naming that file by `ProjectLoader::guarded`.
class SchemaFault : public std::runtime_error {
public:
    SchemaFault(const YAML::Mark& mark, const std::string& message)
        : std::runtime_error(message), mark_(mark) {}
    [[nodiscard]] const YAML::Mark& mark() const noexcept { return mark_; }

private:
    YAML::Mark mark_;
};

[[noreturn]] void fault(const YAML::Node& at, const std::string& message) {
    throw SchemaFault(at.Mark(), message);
}

[[nodiscard]] std::string located(const std::string& file,
                                  const YAML::Mark& mark,
                                  std::string_view message) {
    if (mark.is_null()) {
        return std::format("{}: {}", file, message);
    }
    return std::format("{}:{}:{}: {}", file, mark.line + 1, mark.column + 1, message);
}

/// Request bodies are converted to JSON by walking the YAML tree, and yaml-cpp re-walks an
/// aliased node on every use, so the depth cap alone does not bound the work.
constexpr std::size_t kMaxBodyJsonNodes = 1'000'000;

struct ParseCtx {
    std::string file;
    std::vector<std::string>* warnings{nullptr};
    std::size_t jsonNodesLeft{kMaxBodyJsonNodes};

    void warn(const YAML::Mark& mark, std::string_view message) const {
        if (warnings != nullptr) {
            warnings->push_back(located(file, mark, message));
        }
    }
};

[[nodiscard]] bool present(const YAML::Node& node) {
    return node && !node.IsNull();
}

[[nodiscard]] std::string describe(const YAML::Node& node) {
    if (node.IsMap()) {
        return "a map";
    }
    if (node.IsSequence()) {
        return "a list";
    }
    return "'" + node.Scalar() + "'";
}

void warnUnknownKeys(const ParseCtx& ctx,
                     const YAML::Node& node,
                     std::span<const std::string_view> known,
                     std::string_view where) {
    for (const auto& kv : node) {
        const auto key = kv.first.Scalar();
        if (std::ranges::find(known, key) == known.end()) {
            ctx.warn(kv.first.Mark(), std::format("unknown key '{}' in {}; ignored", key, where));
        }
    }
}

void warnUnknownKeys(const ParseCtx& ctx,
                     const YAML::Node& node,
                     std::initializer_list<std::string_view> known,
                     std::string_view where) {
    warnUnknownKeys(ctx, node, std::span{known.begin(), known.size()}, where);
}

[[nodiscard]] std::string readString(const YAML::Node& parent,
                                     const char* key,
                                     std::string fallback = {}) {
    const auto node = parent[key];
    if (!present(node)) {
        return fallback;
    }
    if (!node.IsScalar()) {
        fault(node, std::format("'{}' must be a string, got {}", key, describe(node)));
    }
    return node.Scalar();
}

[[nodiscard]] std::optional<std::string> readOptString(const YAML::Node& parent, const char* key) {
    if (!present(parent[key])) {
        return std::nullopt;
    }
    return readString(parent, key);
}

[[nodiscard]] int toInt(const YAML::Node& node, std::string_view key) {
    int value{};
    if (!node.IsScalar() || !YAML::convert<int>::decode(node, value)) {
        fault(node, std::format("'{}' must be an integer, got {}", key, describe(node)));
    }
    return value;
}

[[nodiscard]] int readInt(const YAML::Node& parent, const char* key, int fallback) {
    const auto node = parent[key];
    return present(node) ? toInt(node, key) : fallback;
}

[[nodiscard]] bool readBool(const YAML::Node& parent, const char* key, bool fallback) {
    const auto node = parent[key];
    if (!present(node)) {
        return fallback;
    }
    bool value{};
    if (!node.IsScalar() || !YAML::convert<bool>::decode(node, value)) {
        fault(node, std::format("'{}' must be true or false, got {}", key, describe(node)));
    }
    return value;
}

[[nodiscard]] YAML::Node mapAt(const YAML::Node& parent, const char* key) {
    auto node = parent[key];
    if (present(node) && !node.IsMap()) {
        fault(node, std::format("'{}' must be a map, got {}", key, describe(node)));
    }
    return node;
}

[[nodiscard]] YAML::Node seqAt(const YAML::Node& parent, const char* key) {
    auto node = parent[key];
    if (present(node) && !node.IsSequence()) {
        fault(node, std::format("'{}' must be a list, got {}", key, describe(node)));
    }
    return node;
}

template <typename E>
struct EnumName {
    std::string_view name;
    E value;
};

[[nodiscard]] std::string lowerAscii(std::string_view text) {
    std::string out{text};
    std::ranges::transform(
        out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

template <typename E>
[[nodiscard]] std::optional<E> lookupEnum(std::span<const EnumName<E>> table,
                                          std::string_view text,
                                          bool caseInsensitive) {
    const auto wanted = caseInsensitive ? lowerAscii(text) : std::string{text};
    for (const auto& entry : table) {
        if ((caseInsensitive ? lowerAscii(entry.name) : std::string{entry.name}) == wanted) {
            return entry.value;
        }
    }
    return std::nullopt;
}

template <typename E>
[[nodiscard]] std::string enumNames(std::span<const EnumName<E>> table) {
    std::string names;
    for (const auto& entry : table) {
        names += names.empty() ? "" : ", ";
        names += entry.name;
    }
    return names;
}

template <typename E>
[[nodiscard]] E readEnum(const YAML::Node& parent,
                         const char* key,
                         std::span<const EnumName<E>> table,
                         E fallback,
                         bool caseInsensitive = false) {
    const auto node = parent[key];
    if (!present(node)) {
        return fallback;
    }
    if (!node.IsScalar()) {
        fault(node, std::format("'{}' must be a string, got {}", key, describe(node)));
    }
    if (auto value = lookupEnum(table, node.Scalar(), caseInsensitive)) {
        return *value;
    }
    fault(
        node,
        std::format("unknown {} '{}'; accepted values: {}", key, node.Scalar(), enumNames(table)));
}

constexpr std::array<EnumName<HttpMethod>, 7> kMethods{{
    {"GET", HttpMethod::Get},
    {"POST", HttpMethod::Post},
    {"PUT", HttpMethod::Put},
    {"PATCH", HttpMethod::Patch},
    {"DELETE", HttpMethod::Delete},
    {"HEAD", HttpMethod::Head},
    {"OPTIONS", HttpMethod::Options},
}};

[[nodiscard]] HttpMethod readMethod(const YAML::Node& parent, HttpMethod fallback) {
    return readEnum<HttpMethod>(parent, "method", kMethods, fallback, /*caseInsensitive=*/true);
}

[[nodiscard]] std::optional<long long> parseNonNegative(std::string_view digits) {
    long long value{};
    const auto* first = digits.data();
    const auto* last = first + digits.size();
    const auto fc = std::from_chars(first, last, value);
    if (digits.empty() || fc.ec != std::errc{} || fc.ptr != last || value < 0) {
        return std::nullopt;
    }
    return value;
}

/// `<digits>` (seconds) or `<digits>` + one of ms, s, m, h, d; nullopt for anything else.
[[nodiscard]] std::optional<std::chrono::milliseconds> parseDurationLiteral(std::string_view s) {
    constexpr std::array<std::pair<std::string_view, long long>, 5> kUnits{{
        {"ms", 1},
        {"s", 1'000},
        {"m", 60'000},
        {"h", 3'600'000},
        {"d", 86'400'000},
    }};
    std::string_view digits = s;
    long long scale = 1'000;
    for (const auto& [suffix, unitScale] : kUnits) {
        if (s.ends_with(suffix)) {
            digits = s.substr(0, s.size() - suffix.size());
            scale = unitScale;
            break;
        }
    }
    const auto value = parseNonNegative(digits);
    if (!value || *value > std::numeric_limits<long long>::max() / scale) {
        return std::nullopt;
    }
    return std::chrono::milliseconds{*value * scale};
}

[[nodiscard]] std::chrono::milliseconds readDuration(const YAML::Node& parent,
                                                     const char* key,
                                                     std::chrono::milliseconds fallback) {
    const auto node = parent[key];
    if (!present(node)) {
        return fallback;
    }
    if (node.IsScalar()) {
        if (auto value = parseDurationLiteral(node.Scalar())) {
            return *value;
        }
    }
    fault(node,
          std::format("'{}' must be a duration such as 500ms, 5s, 1m, 1h or 1d (a bare number "
                      "is seconds), got {}",
                      key,
                      describe(node)));
}

/// `<digits>` (bytes) or `<digits>` + KiB, MiB or GiB; zero and anything else are rejected.
[[nodiscard]] std::optional<std::size_t> parseByteSize(std::string_view s) {
    constexpr std::array<std::pair<std::string_view, long long>, 3> kUnits{{
        {"KiB", 1LL << 10},
        {"MiB", 1LL << 20},
        {"GiB", 1LL << 30},
    }};
    std::string_view digits = s;
    long long scale = 1;
    for (const auto& [suffix, unitScale] : kUnits) {
        if (s.ends_with(suffix)) {
            digits = s.substr(0, s.size() - suffix.size());
            scale = unitScale;
            break;
        }
    }
    const auto value = parseNonNegative(digits);
    if (!value || *value == 0 || *value > std::numeric_limits<long long>::max() / scale) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(*value * scale);
}

[[nodiscard]] std::map<std::string, std::string> readStringMap(const YAML::Node& parent,
                                                               const char* key) {
    std::map<std::string, std::string> result;
    const auto node = mapAt(parent, key);
    if (!present(node)) {
        return result;
    }
    for (const auto& kv : node) {
        auto name = kv.first.as<std::string>();
        if (kv.second.IsNull()) {
            // `key:` with no value (or `key: ~`) is an empty string, not a literal "~".
            result[name] = "";
        } else if (kv.second.IsScalar()) {
            result[name] = kv.second.Scalar();
        } else {
            YAML::Emitter emitter;
            emitter << kv.second;
            result[name] = emitter.c_str();
        }
    }
    return result;
}

/// Read an environment-variable scalar; `key: !secret NAME` becomes `{{secret.NAME}}` (PRD §5.4).
[[nodiscard]] std::string readEnvScalar(const std::string& name, const YAML::Node& scalar) {
    if (scalar.IsNull()) {
        return {};
    }
    if (!scalar.IsScalar()) {
        fault(scalar,
              std::format(
                  "environment variable '{}' must be a string, got {}", name, describe(scalar)));
    }
    // yaml-cpp reports a local tag as "!secret"; implicit tags ("?"/"!") count as untagged.
    if (scalar.Tag() == "!secret") {
        const auto& secret = scalar.Scalar();
        return secret.empty() ? std::string{} : "{{secret." + secret + "}}";
    }
    return scalar.Scalar();
}

constexpr int kMaxYamlDepth = 64;  // prevent stack overflow on malicious input

nlohmann::json yamlScalarToJsonValue(const YAML::Node& scalar) {
    const auto& raw = scalar.Scalar();
    // A quoted scalar (non-specific "!" tag) stays a string, so "01234" or "true" survive in JSON
    // bodies.
    if (scalar.Tag() == "!") {
        return nlohmann::json(raw);
    }
    // Parens, not braces: nlohmann::json{x} would build a one-element array.
    if (raw == "true") {
        return nlohmann::json(true);
    }
    if (raw == "false") {
        return nlohmann::json(false);
    }
    if (raw == "null" || raw == "~") {
        return {nullptr};
    }
    if (!raw.empty() && ((std::isdigit(static_cast<unsigned char>(raw.front())) != 0) ||
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
            double const d = std::stod(raw, &pos);
            if (pos == raw.size()) {
                return nlohmann::json(d);
            }
            // std::stod throws on non-numeric input: treat the scalar as a string.
            // NOLINTNEXTLINE(bugprone-empty-catch)
        } catch (...) {
        }
    }
    return nlohmann::json(raw);
}

nlohmann::json yamlNodeToJsonValue(const YAML::Node& node, int depth, std::size_t& nodesLeft) {
    if (depth > kMaxYamlDepth) {
        throw YAML::Exception(
            node.Mark(), "YAML nesting exceeds maximum depth of " + std::to_string(kMaxYamlDepth));
    }
    if (nodesLeft == 0) {
        throw YAML::Exception(node.Mark(),
                              std::format("request bodies in this file expand to more than {} "
                                          "YAML nodes (each alias use is expanded)",
                                          kMaxBodyJsonNodes));
    }
    --nodesLeft;
    if (!node || node.IsNull()) {
        return {nullptr};
    }
    if (node.IsScalar()) {
        return yamlScalarToJsonValue(node);
    }
    if (node.IsSequence()) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& item : node) {
            arr.push_back(yamlNodeToJsonValue(item, depth + 1, nodesLeft));
        }
        return arr;
    }
    nlohmann::json obj = nlohmann::json::object();
    for (const auto& kv : node) {
        obj[kv.first.as<std::string>()] = yamlNodeToJsonValue(kv.second, depth + 1, nodesLeft);
    }
    return obj;
}

/// JSON string content where each `{{ … }}` placeholder is copied byte for byte, so the
/// placeholder JsonBodyTemplate finds at run time is the one the author wrote.
void appendTemplateString(std::string& out, std::string_view text) {
    out.push_back('"');
    std::size_t index = 0;
    while (index < text.size()) {
        if (text.substr(index, 2) == "{{") {
            if (const auto end = findPlaceholderEnd(text, index)) {
                out.append(text.substr(index, *end + 2 - index));
                index = *end + 2;
                continue;
            }
        }
        out += escapeJsonStringContent(text.substr(index, 1));
        ++index;
    }
    out.push_back('"');
}

void appendTemplateJson(std::string& out, const nlohmann::json& value) {
    if (value.is_object()) {
        out.push_back('{');
        bool first = true;
        for (const auto& [key, item] : value.items()) {
            out += first ? "" : ",";
            first = false;
            appendTemplateString(out, key);
            out.push_back(':');
            appendTemplateJson(out, item);
        }
        out.push_back('}');
    } else if (value.is_array()) {
        out.push_back('[');
        bool first = true;
        for (const auto& item : value) {
            out += first ? "" : ",";
            first = false;
            appendTemplateJson(out, item);
        }
        out.push_back(']');
    } else if (value.is_string()) {
        appendTemplateString(out, value.get_ref<const std::string&>());
    } else {
        out += value.dump();
    }
}

std::string nodeToJsonString(ParseCtx& ctx, const YAML::Node& node) {
    if (!node || node.IsNull()) {
        return "";
    }
    // A scalar body is the raw request body verbatim; the writer emits the stored body as a
    // scalar, so re-encoding it here would add a layer of quotes on every save.
    if (node.IsScalar()) {
        return node.Scalar();
    }
    std::string out;
    appendTemplateJson(out, yamlNodeToJsonValue(node, 0, ctx.jsonNodesLeft));
    return out;
}

constexpr std::array<EnumName<Extraction::Source>, 6> kExtractionSources{{
    {"jsonpath", Extraction::Source::JsonPath},
    {"xpath", Extraction::Source::XPath},
    {"header", Extraction::Source::Header},
    {"status_code", Extraction::Source::StatusCode},
    {"regex", Extraction::Source::Regex},
    {"cookie", Extraction::Source::Cookie},
}};

// Source implied by the path prefix; XPath/Regex need the explicit map form.
Extraction::Source autoDetectExtractionSource(const std::string& path) {
    if (path.starts_with("$.headers.")) {
        return Extraction::Source::Header;
    }
    if (path.starts_with("$.cookies.")) {
        return Extraction::Source::Cookie;
    }
    if (path == "$.status_code") {
        return Extraction::Source::StatusCode;
    }
    return Extraction::Source::JsonPath;
}

std::vector<Extraction> parseExtractions(const ParseCtx& ctx, const YAML::Node& parent) {
    std::vector<Extraction> result;
    const auto node = mapAt(parent, "extract");
    if (!present(node)) {
        return result;
    }
    for (const auto& kv : node) {
        Extraction ext;
        ext.variableName = kv.first.as<std::string>();
        const auto& value = kv.second;
        // A bare scalar path, or `{ path, source }` for sources a prefix cannot express (XPath,
        // Regex).
        if (value.IsMap()) {
            warnUnknownKeys(ctx, value, {"path", "source"}, "extract." + ext.variableName);
            ext.sourcePath = readString(value, "path");
            ext.source = readEnum<Extraction::Source>(
                value, "source", kExtractionSources, autoDetectExtractionSource(ext.sourcePath));
        } else if (value.IsScalar() || value.IsNull()) {
            ext.sourcePath = value.IsNull() ? std::string{} : value.Scalar();
            ext.source = autoDetectExtractionSource(ext.sourcePath);
        } else {
            fault(value,
                  std::format("extract '{}' must be a path or a {{ path, source }} map, got {}",
                              ext.variableName,
                              describe(value)));
        }
        result.push_back(std::move(ext));
    }
    return result;
}

/// The `assert:` block: a list whose items are a bare predicate or `{ expr, name }`.
std::vector<Assertion> parseAssertions(const ParseCtx& ctx, const YAML::Node& parent) {
    std::vector<Assertion> result;
    const auto node = seqAt(parent, "assert");
    if (!present(node)) {
        return result;
    }
    for (const auto& item : node) {
        Assertion assertion;
        if (item.IsMap()) {
            warnUnknownKeys(ctx, item, {"expr", "name"}, "assert");
            assertion.expr = readString(item, "expr");
            assertion.name = readOptString(item, "name");
        } else if (item.IsScalar()) {
            assertion.expr = item.Scalar();
        } else if (!item.IsNull()) {
            fault(item,
                  "an assert item must be a predicate or an { expr, name } map, got " +
                      describe(item));
        }
        if (!assertion.expr.empty()) {
            result.push_back(std::move(assertion));
        }
    }
    return result;
}

/// `expect_status`: one integer or a list of integers.
void readExpectStatus(const YAML::Node& parent,
                      std::optional<int>& single,
                      std::vector<int>& list) {
    const auto node = parent["expect_status"];
    if (!present(node)) {
        return;
    }
    if (node.IsSequence()) {
        for (const auto& item : node) {
            list.push_back(toInt(item, "expect_status"));
        }
    } else {
        single = toInt(node, "expect_status");
    }
}

constexpr std::array<EnumName<InlineAuthType>, 18> kInlineAuthTypes{{
    {"none", InlineAuthType::None},
    {"bearer", InlineAuthType::Bearer},
    {"basic", InlineAuthType::Basic},
    {"apikey", InlineAuthType::ApiKey},
    {"api_key", InlineAuthType::ApiKey},
    {"aws_sigv4", InlineAuthType::AwsSigV4},
    {"awssigv4", InlineAuthType::AwsSigV4},
    {"aws", InlineAuthType::AwsSigV4},
    {"oauth1", InlineAuthType::OAuth1},
    {"oauth_1", InlineAuthType::OAuth1},
    {"oauth2", InlineAuthType::OAuth2},
    {"oauth_2", InlineAuthType::OAuth2},
    {"oauth2_client_credentials", InlineAuthType::OAuth2},
    {"jwt", InlineAuthType::Jwt},
    {"jwt_bearer", InlineAuthType::Jwt},
    {"mtls", InlineAuthType::Mtls},
    {"mutual_tls", InlineAuthType::Mtls},
    {"inherit", InlineAuthType::Inherit},
}};

constexpr std::array<EnumName<bool>, 2> kApiKeyLocations{{
    {"header", false},
    {"query", true},
}};

[[nodiscard]] std::vector<std::string_view> inlineAuthKeys(InlineAuthType type) {
    switch (type) {
        case InlineAuthType::Bearer:
            return {"type", "token"};
        case InlineAuthType::Basic:
            return {"type", "username", "password"};
        case InlineAuthType::ApiKey:
            return {"type", "key", "value", "in"};
        case InlineAuthType::AwsSigV4:
            return {"type", "access_key", "secret_key", "region", "service", "session_token"};
        case InlineAuthType::OAuth1:
            return {"type", "consumer_key", "consumer_secret", "oauth_token", "token_secret"};
        case InlineAuthType::OAuth2:
            return {"type",
                    "grant_type",
                    "token_url",
                    "client_id",
                    "client_secret",
                    "scope",
                    "client_auth",
                    "username",
                    "password",
                    "auth_url",
                    "callback_url",
                    "pkce_method"};
        case InlineAuthType::Jwt:
            return {"type", "algorithm", "secret", "payload"};
        case InlineAuthType::Mtls:
            return {"type", "format", "cert_path", "key_path", "key_password", "ca_cert_path"};
        case InlineAuthType::None:
        case InlineAuthType::Inherit:
            break;
    }
    return {"type"};
}

std::optional<InlineAuth> parseInlineAuth(const ParseCtx& ctx, const YAML::Node& parent) {
    const auto node = mapAt(parent, "auth");
    if (!present(node)) {
        return std::nullopt;
    }
    InlineAuth auth;
    auth.type = readEnum<InlineAuthType>(
        node, "type", kInlineAuthTypes, InlineAuthType::None, /*caseInsensitive=*/true);
    if (!present(node["type"])) {
        ctx.warn(node.Mark(), "'auth' has no 'type'; the block is ignored");
        return std::nullopt;
    }
    warnUnknownKeys(ctx, node, inlineAuthKeys(auth.type), "auth");
    if (auth.type == InlineAuthType::None) {
        return std::nullopt;
    }
    auth.token = readString(node, "token");
    auth.username = readString(node, "username");
    auth.password = readString(node, "password");
    auth.apiKeyName = readString(node, "key");
    auth.apiKeyValue = readString(node, "value");
    auth.apiKeyInQuery = readEnum<bool>(node, "in", kApiKeyLocations, false);
    auth.awsAccessKey = readString(node, "access_key");
    auth.awsSecretKey = readString(node, "secret_key");
    auth.awsRegion = readString(node, "region");
    auth.awsService = readString(node, "service");
    auth.awsSessionToken = readString(node, "session_token");
    auth.oauthConsumerKey = readString(node, "consumer_key");
    auth.oauthConsumerSecret = readString(node, "consumer_secret");
    auth.oauthToken = readString(node, "oauth_token");
    auth.oauthTokenSecret = readString(node, "token_secret");
    auth.oauth2GrantType = readString(node, "grant_type");
    auth.oauth2TokenUrl = readString(node, "token_url");
    auth.oauth2ClientId = readString(node, "client_id");
    auth.oauth2ClientSecret = readString(node, "client_secret");
    auth.oauth2Scope = readString(node, "scope");
    auth.oauth2ClientAuth = readString(node, "client_auth");
    auth.oauth2AuthUrl = readString(node, "auth_url");
    auth.oauth2CallbackUrl = readString(node, "callback_url");
    auth.oauth2PkceMethod = readString(node, "pkce_method");
    // oauth2AccessToken is never read or written: it is an ephemeral runtime secret.
    auth.jwtAlgorithm = readString(node, "algorithm");
    auth.jwtSecret = readString(node, "secret");
    auth.jwtPayload = readString(node, "payload");
    auth.mtlsFormat = readString(node, "format");
    // Same key names as the actor-level mTLS block (parseActor).
    auth.mtlsCertPath = readString(node, "cert_path");
    auth.mtlsKeyPath = readString(node, "key_path");
    auth.mtlsKeyPassword = readString(node, "key_password");
    auth.mtlsCaCertPath = readString(node, "ca_cert_path");
    return auth;
}

/// Parse a `transport:` block (absent → a default `TransportConfig`).
///
///   tls_verify, tls_verify_host: bool          (default true)
///   ca_bundle:                   path          (optional)
///   proxy:                       URL           (optional)
///   connect_timeout:             duration      (default 5s)
///   max_response_size:           bytes, or KiB/MiB/GiB (default 64MiB)
TransportConfig parseTransport(const ParseCtx& ctx, const YAML::Node& parent) {
    TransportConfig out;
    const auto node = mapAt(parent, "transport");
    if (!present(node)) {
        return out;
    }
    warnUnknownKeys(ctx,
                    node,
                    {"tls_verify",
                     "tls_verify_host",
                     "ca_bundle",
                     "proxy",
                     "connect_timeout",
                     "max_response_size"},
                    "transport");
    out.tlsVerify = readBool(node, "tls_verify", true);
    out.tlsVerifyHost = readBool(node, "tls_verify_host", true);
    if (auto path = readString(node, "ca_bundle"); !path.empty()) {
        out.caBundlePath = std::move(path);
    }
    if (auto proxy = readString(node, "proxy"); !proxy.empty()) {
        out.proxy = std::move(proxy);
    }
    out.connectTimeout = readDuration(node, "connect_timeout", out.connectTimeout);
    if (const auto size = node["max_response_size"]; present(size)) {
        const auto bytes = size.IsScalar() ? parseByteSize(size.Scalar()) : std::nullopt;
        if (!bytes) {
            fault(size,
                  "'max_response_size' must be a positive byte count or a size in KiB, MiB or "
                  "GiB such as 16MiB, got " +
                      describe(size));
        }
        out.maxResponseBytes = *bytes;
    }
    return out;
}

// A hook value is a file reference when it looks like a relative path to a .js/.mjs file; shared so
// the parser can load it and remember it for a faithful round-trip.
[[nodiscard]] bool hookValueIsFileRef(std::string_view s) {
    if (s.starts_with("./") || s.starts_with("../")) {
        return true;
    }
    if (s.find('\n') != std::string_view::npos) {
        return false;
    }
    if (s.find('{') != std::string_view::npos) {
        return false;
    }
    if (s.find('(') != std::string_view::npos) {
        return false;
    }
    if (s.find('=') != std::string_view::npos) {
        return false;
    }
    return s.ends_with(".js") || s.ends_with(".mjs");
}

[[nodiscard]] std::expected<std::string, ReqloomError> hookError(std::string message) {
    return std::unexpected(
        ReqloomError{ErrorCode::SchemaInvalid, ErrorClass::Schema, std::move(message)});
}

// Resolve a hook value: a relative .js/.mjs path is loaded (canonicalised against baseDir, rejected
// if outside root, capped at 1 MiB); anything else is inline JS.
[[nodiscard]] std::expected<std::string, ReqloomError> resolveHookScript(const std::string& value,
                                                                         const fs::path& baseDir) {
    if (value.empty() || !hookValueIsFileRef(value)) {
        return value;
    }

    const fs::path raw{value};
    // is_absolute() is host-dependent (false for "/etc/passwd.js" on Windows), so reject any
    // leading separator or drive prefix on every OS.
    const auto hasRootedPrefix = [](std::string_view s) {
        if (s.starts_with('/') || s.starts_with('\\')) {
            return true;
        }
        return s.size() >= 3 && std::isalpha(static_cast<unsigned char>(s[0])) && s[1] == ':' &&
               (s[2] == '\\' || s[2] == '/');
    };
    if (raw.is_absolute() || hasRootedPrefix(value)) {
        return hookError("hook script path must be relative to the project root: " + value);
    }

    std::error_code ec;
    const auto canonical = fs::weakly_canonical(baseDir / raw, ec);
    if (ec) {
        return hookError("hook script path is not resolvable: " + value + " (" + ec.message() +
                         ")");
    }

    // fs::canonical (not weakly_canonical) for the base: symlinks must be fully resolved for
    // the prefix comparison, which must also end at a separator (proj vs proj-evil).
    const auto canonicalBase = fs::canonical(baseDir, ec);
    if (ec) {
        return hookError("could not canonicalise project root: " + ec.message());
    }
    {
        const auto canonStr = canonical.lexically_normal().string();
        const auto baseStr = canonicalBase.lexically_normal().string();
        const bool contained =
            canonStr.size() >= baseStr.size() && canonStr.substr(0, baseStr.size()) == baseStr &&
            (canonStr.size() == baseStr.size() || canonStr[baseStr.size()] == '/' ||
             canonStr[baseStr.size()] == fs::path::preferred_separator);
        if (!contained) {
            return hookError("hook script path escapes the project root: " + value);
        }
    }

    if (!fs::exists(canonical, ec) || ec) {
        return hookError("hook script not found: " + canonical.string());
    }
    if (!fs::is_regular_file(canonical, ec) || ec) {
        return hookError("hook script is not a regular file: " + canonical.string());
    }

    constexpr std::uintmax_t kMaxHookBytes = std::uintmax_t{1} * 1024 * 1024;  // 1 MiB
    const auto size = fs::file_size(canonical, ec);
    if (ec) {
        return hookError("could not stat hook script " + canonical.string() + ": " + ec.message());
    }
    if (size > kMaxHookBytes) {
        return hookError("hook script exceeds 1 MiB cap: " + canonical.string());
    }

    std::ifstream in(canonical, std::ios::binary);
    if (!in) {
        return hookError("hook script could not be opened: " + canonical.string());
    }
    std::string contents(static_cast<std::size_t>(size), '\0');
    in.read(contents.data(), static_cast<std::streamsize>(size));
    if (in.gcount() != static_cast<std::streamsize>(size)) {
        return hookError("hook script read truncated: " + canonical.string());
    }
    return contents;
}

constexpr std::array<EnumName<AuthStrategy>, 11> kStrategies{{
    {"simple", AuthStrategy::Simple},
    {"chain", AuthStrategy::Chain},
    {"basic", AuthStrategy::Basic},
    {"api_key", AuthStrategy::ApiKey},
    {"oauth2_client_credentials", AuthStrategy::OAuth2ClientCredentials},
    {"oauth2_password", AuthStrategy::OAuth2Password},
    {"oauth1", AuthStrategy::OAuth1},
    {"aws_sigv4", AuthStrategy::AwsSigV4},
    {"bearer", AuthStrategy::Bearer},
    {"jwt", AuthStrategy::Jwt},
    {"mtls", AuthStrategy::Mtls},
}};

struct StrategyKeys {
    std::vector<std::string_view> required;
    std::vector<std::string_view> optional;
};

[[nodiscard]] StrategyKeys strategyKeys(AuthStrategy strategy) {
    switch (strategy) {
        case AuthStrategy::Basic:
            return {{"username", "password"}, {}};
        case AuthStrategy::ApiKey:
            return {{"key"}, {"location", "name"}};
        case AuthStrategy::OAuth2ClientCredentials:
            return {{"token_url", "client_id", "client_secret"}, {"scope"}};
        case AuthStrategy::OAuth2Password:
            return {{"token_url", "client_id", "client_secret", "username", "password"}, {"scope"}};
        case AuthStrategy::OAuth1:
            return {{"consumer_key", "consumer_secret"}, {"token", "token_secret", "realm"}};
        case AuthStrategy::AwsSigV4:
            return {{"access_key", "secret_key", "region", "service"}, {"session_token"}};
        case AuthStrategy::Bearer:
            return {{"token"}, {}};
        case AuthStrategy::Jwt:
            return {{"secret", "payload"}, {"algorithm"}};
        case AuthStrategy::Mtls:
            return {{"cert_path"}, {"format", "key_path", "key_password", "ca_cert_path"}};
        case AuthStrategy::Simple:
        case AuthStrategy::Chain:
            break;
    }
    return {};
}

constexpr std::array<std::string_view, 7> kAuthStepKeys{
    "id", "method", "path", "headers", "body", "expect_status", "extract"};

AuthStep parseAuthStep(ParseCtx& ctx, const YAML::Node& node, bool chainStep) {
    AuthStep step;
    step.id = chainStep ? readString(node, "id") : std::string{"login"};
    step.method = readMethod(node, HttpMethod::Post);
    step.pathTemplate = readString(node, "path");
    step.headers = readStringMap(node, "headers");
    if (const auto body = node["body"]) {
        step.bodyTemplate = nodeToJsonString(ctx, body);
    }
    if (present(node["expect_status"])) {
        step.expectStatus = toInt(node["expect_status"], "expect_status");
    }
    step.extractions = parseExtractions(ctx, node);
    return step;
}

void parseActorAuth(ParseCtx& ctx, Actor& actor, const YAML::Node& auth) {
    actor.strategy = readEnum<AuthStrategy>(auth, "strategy", kStrategies, AuthStrategy::Simple);
    const auto where = std::format("actor '{}' auth", actor.id.value);

    if (actor.strategy == AuthStrategy::Chain) {
        warnUnknownKeys(ctx, auth, {"strategy", "steps"}, where);
        const auto steps = seqAt(auth, "steps");
        if (present(steps)) {
            for (const auto& stepNode : steps) {
                if (!stepNode.IsMap()) {
                    fault(stepNode, "an auth step must be a map, got " + describe(stepNode));
                }
                warnUnknownKeys(ctx, stepNode, kAuthStepKeys, where + " step");
                actor.authSteps.push_back(parseAuthStep(ctx, stepNode, /*chainStep=*/true));
            }
        }
        return;
    }
    if (actor.strategy == AuthStrategy::Simple) {
        std::vector<std::string_view> known{kAuthStepKeys.begin() + 1, kAuthStepKeys.end()};
        known.emplace_back("strategy");
        warnUnknownKeys(ctx, auth, known, where);
        actor.authSteps.push_back(parseAuthStep(ctx, auth, /*chainStep=*/false));
        return;
    }
    // Config-map strategies: values may contain {{X.y}} references resolved at auth time.
    const auto keys = strategyKeys(actor.strategy);
    std::vector<std::string_view> known{"strategy"};
    known.insert(known.end(), keys.required.begin(), keys.required.end());
    known.insert(known.end(), keys.optional.begin(), keys.optional.end());
    warnUnknownKeys(ctx, auth, known, where);
    for (const auto key : keys.required) {
        const std::string name{key};
        actor.authConfig[name] = readString(auth, name.c_str());
    }
    for (const auto key : keys.optional) {
        const std::string name{key};
        if (auto value = readOptString(auth, name.c_str())) {
            actor.authConfig[name] = std::move(*value);
        }
    }
}

void parseSession(ParseCtx& ctx, Actor& actor, const YAML::Node& node) {
    const auto session = mapAt(node, "session");
    if (!present(session)) {
        return;
    }
    warnUnknownKeys(ctx, session, {"ttl", "refresh"}, "session");
    actor.sessionTtl = std::chrono::duration_cast<std::chrono::seconds>(
        readDuration(session, "ttl", actor.sessionTtl));
    const auto r = mapAt(session, "refresh");
    if (!present(r)) {
        return;
    }
    warnUnknownKeys(
        ctx, r, {"method", "path", "headers", "body", "expect_status", "extract"}, "refresh");
    SessionRefresh refresh;
    refresh.method = readMethod(r, HttpMethod::Post);
    refresh.pathTemplate = readString(r, "path");
    refresh.headers = readStringMap(r, "headers");
    if (const auto body = r["body"]) {
        refresh.bodyTemplate = nodeToJsonString(ctx, body);
    }
    // When expect_status is unset, runRefresh treats any 2xx as success.
    readExpectStatus(r, refresh.expectStatus, refresh.expectStatusList);
    refresh.extractions = parseExtractions(ctx, r);
    actor.refresh = std::move(refresh);
}

Actor parseActor(ParseCtx& ctx, const std::string& actorId, const YAML::Node& node, int line) {
    if (!node.IsMap()) {
        fault(node, std::format("actor '{}' must be a map, got {}", actorId, describe(node)));
    }
    warnUnknownKeys(ctx,
                    node,
                    {"name", "description", "auth", "session", "inject"},
                    std::format("actor '{}'", actorId));
    Actor actor;
    actor.id = ActorId{actorId};
    actor.source = SourceLocation{ctx.file, line};
    actor.description = readString(node, "description");

    if (const auto auth = mapAt(node, "auth"); present(auth)) {
        parseActorAuth(ctx, actor, auth);
    }
    parseSession(ctx, actor, node);
    if (const auto inject = mapAt(node, "inject"); present(inject)) {
        warnUnknownKeys(ctx, inject, {"headers"}, "inject");
        actor.inject.headers = readStringMap(inject, "headers");
    }
    return actor;
}

constexpr std::array<EnumName<Provenance::Source>, 12> kProvenanceSources{{
    {"hand_written", Provenance::Source::HandWritten},
    {"openapi_import", Provenance::Source::OpenApiImport},
    {"postman_import", Provenance::Source::PostmanImport},
    {"bruno_import", Provenance::Source::BrunoImport},
    {"insomnia_import", Provenance::Source::InsomniaImport},
    {"har_import", Provenance::Source::HarImport},
    {"ai_import", Provenance::Source::AiImport},
    {"apidog_import", Provenance::Source::ApidogImport},
    {"hoppscotch_import", Provenance::Source::HoppscotchImport},
    {"thunder_client_import", Provenance::Source::ThunderClientImport},
    {"httpie_import", Provenance::Source::HttpieImport},
    {"http_file_import", Provenance::Source::HttpFileImport},
}};

constexpr std::array<EnumName<Provenance::VerifiedAgainst>, 7> kVerifiedAgainst{{
    {"none", Provenance::VerifiedAgainst::None},
    {"openapi_example", Provenance::VerifiedAgainst::OpenApiExample},
    {"postman_response", Provenance::VerifiedAgainst::PostmanResponse},
    {"insomnia_response", Provenance::VerifiedAgainst::InsomniaResponse},
    {"har_entry", Provenance::VerifiedAgainst::HarEntry},
    {"synthetic", Provenance::VerifiedAgainst::Synthetic},
    {"live_capture", Provenance::VerifiedAgainst::LiveCapture},
}};

// The `_provenance` block is metadata the runtime ignores, so an unknown label is a warning,
// not a load error; it must survive parse→write→parse so a save keeps the audit trail.
Provenance parseProvenance(const ParseCtx& ctx, const YAML::Node& node) {
    Provenance prov;
    warnUnknownKeys(ctx,
                    node,
                    {"source", "verified_against", "model", "imported_at", "evidence"},
                    "_provenance");
    if (const auto source = node["source"]; present(source)) {
        const auto text = readString(node, "source");
        if (auto known = lookupEnum<Provenance::Source>(kProvenanceSources, text, false)) {
            prov.source = *known;
        } else {
            ctx.warn(source.Mark(),
                     std::format("unknown _provenance source '{}'; kept as written (known: {})",
                                 text,
                                 enumNames<Provenance::Source>(kProvenanceSources)));
            prov.source = Provenance::Source::Unknown;
            prov.unknownSource = text;
        }
    }
    if (const auto verified = node["verified_against"]; present(verified)) {
        const auto text = readString(node, "verified_against");
        if (auto known = lookupEnum<Provenance::VerifiedAgainst>(kVerifiedAgainst, text, false)) {
            prov.verifiedAgainst = *known;
        } else {
            ctx.warn(verified.Mark(),
                     std::format("unknown _provenance verified_against '{}'; read as none", text));
        }
    }
    prov.model = readOptString(node, "model");
    prov.importedAt = readOptString(node, "imported_at");
    prov.evidence = readStringMap(node, "evidence");
    return prov;
}

constexpr std::array<std::string_view, 21> kOperationKeys{
    "method",        "path",    "actor",     "auth",          "headers",
    "query_params",  "body",    "body_form", "expect_status", "poll_until",
    "for_each",      "extract", "assert",    "depends_on",    "pre_request",
    "post_response", "retry",   "timeout",   "force",         "failure_message",
    "_provenance"};

/// `failure_message: { show: [<JSONPath>, ...] }`; nullopt when the block or `show` is absent.
/// Paths are validated with the rest of the project (DependencyResolver).
std::optional<std::vector<std::string>> parseFailureMessage(const ParseCtx& ctx,
                                                            const YAML::Node& parent) {
    const auto node = mapAt(parent, "failure_message");
    if (!present(node)) {
        return std::nullopt;
    }
    warnUnknownKeys(ctx, node, {"show"}, "failure_message");
    const auto show = seqAt(node, "show");
    if (!present(show)) {
        return std::nullopt;
    }
    std::vector<std::string> paths;
    for (const auto& item : show) {
        if (!item.IsScalar()) {
            fault(item,
                  "a failure_message.show entry must be a JSONPath string, got " + describe(item));
        }
        paths.push_back(item.Scalar());
    }
    return paths;
}

PollUntil parsePollUntil(const ParseCtx& ctx, const YAML::Node& p) {
    warnUnknownKeys(ctx,
                    p,
                    {"method",
                     "path",
                     "actor",
                     "success_when",
                     "fail_when",
                     "interval",
                     "backoff",
                     "timeout",
                     "max_attempts"},
                    "poll_until");
    PollUntil poll;
    poll.method = readMethod(p, HttpMethod::Get);
    poll.pathTemplate = readString(p, "path");
    if (auto actor = readOptString(p, "actor")) {
        poll.actor = ActorId{std::move(*actor)};
    }
    poll.successWhen = readString(p, "success_when");
    poll.failWhen = readOptString(p, "fail_when");
    poll.interval = readDuration(p, "interval", poll.interval);
    if (const auto b = mapAt(p, "backoff"); present(b)) {
        warnUnknownKeys(ctx, b, {"base", "max"}, "poll_until.backoff");
        if (present(b["base"])) {
            poll.backoffBase = readDuration(b, "base", std::chrono::milliseconds{500});
        }
        poll.backoffMax = readDuration(b, "max", poll.backoffMax);
    }
    poll.timeout = readDuration(p, "timeout", poll.timeout);
    poll.maxAttempts = readInt(p, "max_attempts", poll.maxAttempts);
    return poll;
}

void parseRetry(const ParseCtx& ctx, Operation& op, const YAML::Node& retryNode) {
    warnUnknownKeys(ctx, retryNode, {"max", "backoff", "non_idempotent"}, "retry");
    op.retry.maxAttempts = readInt(retryNode, "max", op.retry.maxAttempts);
    if (present(retryNode["backoff"])) {
        op.retry.baseBackoff = std::chrono::milliseconds{toInt(retryNode["backoff"], "backoff")};
    }
    // Opting a POST/PATCH into re-sends is a safety decision, so a typo must fail the load.
    op.retry.nonIdempotent = readBool(retryNode, "non_idempotent", false);
}

void parseHook(const YAML::Node& opNode,
               const char* key,
               const OperationId& opId,
               const fs::path& baseDir,
               std::optional<std::string>& script,
               std::optional<std::string>& scriptRef) {
    const auto node = opNode[key];
    if (!present(node)) {
        return;
    }
    const auto raw = readString(opNode, key);
    auto resolved = resolveHookScript(raw, baseDir);
    if (!resolved) {
        fault(node, std::format("operation '{}' {}: {}", opId.value, key, resolved.error().detail));
    }
    script = std::move(*resolved);
    if (hookValueIsFileRef(raw)) {
        scriptRef = raw;
    }
}

void parseOperationFlow(ParseCtx& ctx, Operation& op, const YAML::Node& opNode) {
    if (const auto p = mapAt(opNode, "poll_until"); present(p)) {
        op.pollUntil = parsePollUntil(ctx, p);
    }
    if (const auto fe = mapAt(opNode, "for_each"); present(fe)) {
        warnUnknownKeys(ctx, fe, {"over", "continue_on_error"}, "for_each");
        const auto over = readString(fe, "over");
        if (over.empty()) {
            fault(
                fe,
                std::format("operation '{}' for_each needs 'over' naming a resource", op.id.value));
        }
        ForEach forEach{ResourceId{over}};
        forEach.continueOnError = readBool(fe, "continue_on_error", false);
        op.forEach = forEach;
    }
    if (const auto deps = opNode["depends_on"]; present(deps)) {
        if (deps.IsScalar()) {
            op.explicitDependencies.push_back(OperationId{deps.Scalar()});
        } else if (deps.IsSequence()) {
            for (const auto& dep : deps) {
                if (!dep.IsScalar()) {
                    fault(dep, "a depends_on item must be an operation id, got " + describe(dep));
                }
                op.explicitDependencies.push_back(OperationId{dep.Scalar()});
            }
        } else {
            fault(deps,
                  "'depends_on' must be an operation id or a list of them, got " + describe(deps));
        }
    }
    if (const auto retryNode = mapAt(opNode, "retry"); present(retryNode)) {
        parseRetry(ctx, op, retryNode);
    }
    if (present(opNode["timeout"])) {
        op.timeout = std::chrono::milliseconds{toInt(opNode["timeout"], "timeout")};
    }
    op.force = readBool(opNode, "force", false);
}

Operation parseOperation(ParseCtx& ctx,
                         const std::string& resourceId,
                         const YAML::Node& key,
                         const YAML::Node& opNode,
                         const fs::path& baseDir) {
    const auto opName = key.as<std::string>();
    Operation op;
    op.id = OperationId{std::format("{}.{}", resourceId, opName)};
    if (!opNode.IsMap()) {
        fault(opNode,
              std::format("operation '{}' must be a map, got {}", op.id.value, describe(opNode)));
    }
    warnUnknownKeys(ctx, opNode, kOperationKeys, std::format("operation '{}'", op.id.value));
    op.resource = ResourceId{resourceId};
    op.source = SourceLocation{ctx.file, key.Mark().line + 1};
    op.method = readMethod(opNode, HttpMethod::Get);
    op.pathTemplate = readString(opNode, "path");
    if (auto actor = readOptString(opNode, "actor")) {
        op.actor = ActorId{std::move(*actor)};
    }
    op.inlineAuth = parseInlineAuth(ctx, opNode);
    op.headers = readStringMap(opNode, "headers");
    op.queryParams = readStringMap(opNode, "query_params");
    if (const auto body = opNode["body"]) {
        op.bodyTemplate = nodeToJsonString(ctx, body);
    }
    if (present(opNode["body_form"])) {
        op.bodyForm = readStringMap(opNode, "body_form");
    }
    // The list form is required with poll_until (202 alongside the poll's 200).
    readExpectStatus(opNode, op.expectStatus, op.expectStatusList);
    op.extractions = parseExtractions(ctx, opNode);
    op.assertions = parseAssertions(ctx, opNode);
    op.failureMessageShow = parseFailureMessage(ctx, opNode);
    parseOperationFlow(ctx, op, opNode);
    parseHook(opNode, "pre_request", op.id, baseDir, op.preRequestScript, op.preRequestScriptRef);
    parseHook(
        opNode, "post_response", op.id, baseDir, op.postResponseScript, op.postResponseScriptRef);
    if (const auto prov = mapAt(opNode, "_provenance"); present(prov)) {
        op.provenance = parseProvenance(ctx, prov);
    }
    return op;
}

Resource parseResource(ParseCtx& ctx,
                       const std::string& resourceId,
                       const YAML::Node& node,
                       int line,
                       const fs::path& baseDir) {
    if (!node.IsMap()) {
        fault(node, std::format("resource '{}' must be a map, got {}", resourceId, describe(node)));
    }
    warnUnknownKeys(
        ctx, node, {"name", "description", "operations"}, std::format("resource '{}'", resourceId));
    Resource resource;
    resource.id = ResourceId{resourceId};
    resource.source = SourceLocation{ctx.file, line};
    resource.description = readString(node, "description");

    const auto ops = mapAt(node, "operations");
    if (!present(ops)) {
        return resource;
    }
    for (const auto& kv : ops) {
        auto op = parseOperation(ctx, resourceId, kv.first, kv.second, baseDir);
        resource.operations[kv.first.as<std::string>()] = std::move(op);
    }
    return resource;
}

// Cap raw document size before yaml-cpp: the YAML layer is attacker-controlled and 8 MiB is far
// above any real schema.
constexpr std::uintmax_t kMaxYamlBytes = std::uintmax_t{8} * 1024 * 1024;

struct LoadedYaml {
    YAML::Node root;
    std::size_t hash{};
};

[[nodiscard]] ReqloomError schemaError(ErrorCode code, std::string detail) {
    return ReqloomError{code, ErrorClass::Schema, std::move(detail)};
}

[[nodiscard]] std::expected<LoadedYaml, ReqloomError> loadYamlCapped(const fs::path& file) {
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    if (ec) {
        return std::unexpected(
            schemaError(ErrorCode::YamlParse,
                        "could not stat schema file " + file.string() + ": " + ec.message()));
    }
    if (size > kMaxYamlBytes) {
        return std::unexpected(
            schemaError(ErrorCode::YamlParse, "schema file exceeds 8 MiB cap: " + file.string()));
    }
    std::ifstream in{file, std::ios::binary};
    std::string bytes(static_cast<std::size_t>(size), '\0');
    in.read(bytes.data(), static_cast<std::streamsize>(size));
    if (!in || in.gcount() != static_cast<std::streamsize>(size)) {
        return std::unexpected(
            schemaError(ErrorCode::YamlParse, "could not read schema file " + file.string()));
    }
    try {
        return LoadedYaml{YAML::Load(bytes), schemaFileHash(bytes)};
    } catch (const YAML::Exception& e) {
        return std::unexpected(
            schemaError(ErrorCode::YamlParse, located(file.string(), e.mark, e.msg)));
    }
}

/// Files an `imports` pattern names: `dir/*.yaml` / `dir/*.yml`, or one literal path.
[[nodiscard]] std::expected<std::vector<fs::path>, ReqloomError> resolveGlob(
    const fs::path& baseDir, const std::string& pattern) {
    std::vector<fs::path> results;
    const auto dir = baseDir / fs::path(pattern).parent_path();
    const auto name = fs::path(pattern).filename().string();
    std::error_code ec;

    if (name != "*.yaml" && name != "*.yml") {
        const auto file = baseDir / pattern;
        if (fs::is_regular_file(file, ec)) {
            results.push_back(file);
        }
        return results;
    }
    if (!fs::is_directory(dir, ec)) {
        return results;
    }
    const auto readFailure = [&](const std::error_code& err) {
        return std::unexpected(schemaError(
            ErrorCode::SchemaInvalid,
            std::format("could not read import folder {}: {}", dir.string(), err.message())));
    };
    fs::directory_iterator it{dir, ec};
    if (ec) {
        return readFailure(ec);
    }
    for (; it != fs::directory_iterator{}; it.increment(ec)) {
        if (ec) {
            return readFailure(ec);
        }
        const auto ext = it->path().extension().string();
        std::error_code typeEc;
        if (it->is_regular_file(typeEc) && (ext == ".yaml" || ext == ".yml")) {
            results.push_back(it->path());
        }
    }
    if (ec) {
        return readFailure(ec);
    }
    std::ranges::sort(results);
    return results;
}

enum class SubFileKind : std::uint8_t { Actor, Resource, Environment, Other };

[[nodiscard]] SubFileKind subFileKind(const std::string& relPath) {
    const auto under = [&](std::string_view dir) {
        return relPath.starts_with(std::string{dir} + "/") ||
               relPath.starts_with(std::string{dir} + "\\");
    };
    if (under("actors")) {
        return SubFileKind::Actor;
    }
    if (under("resources")) {
        return SubFileKind::Resource;
    }
    if (under("environments")) {
        return SubFileKind::Environment;
    }
    return SubFileKind::Other;
}

/// An entity file is flat (`name:` plus fields, or fields only) or wrapped (`<id>: {fields}`).
/// A single top-level key counts as wrapped only when it is not a field of the file kind and
/// holds a map, so a flat file with one field keeps its file-stem id.
struct EntityBody {
    std::string id;
    YAML::Node body;
    int line{1};
};

[[nodiscard]] EntityBody unwrapEntity(const YAML::Node& doc,
                                      const fs::path& file,
                                      std::span<const std::string_view> fields) {
    if (!doc.IsMap()) {
        fault(doc, "an entity file must be a map, got " + describe(doc));
    }
    if (present(doc["name"])) {
        return {readString(doc, "name"), doc, 1};
    }
    if (doc.size() == 1) {
        const auto it = doc.begin();
        const auto key = it->first.as<std::string>();
        if (std::ranges::find(fields, key) == fields.end() && it->second.IsMap()) {
            return {key, it->second, it->first.Mark().line + 1};
        }
    }
    return {file.stem().string(), doc, 1};
}

constexpr std::array<std::string_view, 5> kActorFields{
    "name", "description", "auth", "session", "inject"};
constexpr std::array<std::string_view, 3> kResourceFields{"name", "description", "operations"};

constexpr std::array<std::string_view, 13> kRootKeys{"version",
                                                     "name",
                                                     "id",
                                                     "default_environment",
                                                     "allowed_env",
                                                     "auth",
                                                     "latency_slo",
                                                     "imports",
                                                     "actors",
                                                     "resources",
                                                     "environment",
                                                     "transport",
                                                     "failure_message"};

class ProjectLoader {
public:
    explicit ProjectLoader(fs::path rootYaml)
        : rootYaml_(std::move(rootYaml)), baseDir_(rootYaml_.parent_path()) {}

    [[nodiscard]] std::expected<Project, ReqloomError> load();

private:
    template <typename Body>
    [[nodiscard]] std::optional<ReqloomError> guarded(const std::string& file, Body&& body) {
        try {
            std::forward<Body>(body)();
        } catch (const SchemaFault& f) {
            return schemaError(ErrorCode::SchemaInvalid, located(file, f.mark(), f.what()));
        } catch (const YAML::Exception& e) {
            return schemaError(ErrorCode::YamlParse, located(file, e.mark, e.msg));
        } catch (const fs::filesystem_error& e) {
            return schemaError(ErrorCode::SchemaInvalid, file + ": " + e.what());
        }
        return std::nullopt;
    }

    [[nodiscard]] ParseCtx contextFor(const fs::path& file) {
        return ParseCtx{file.string(), &project_.loadWarnings, kMaxBodyJsonNodes};
    }

    void readRootSettings(ParseCtx& ctx, const YAML::Node& root);
    [[nodiscard]] std::optional<ReqloomError> processImports(ParseCtx& ctx, const YAML::Node& root);
    [[nodiscard]] std::optional<ReqloomError> loadSubFile(const fs::path& file, SubFileKind kind);
    void addActor(const YAML::Node& at, Actor actor);
    void addResource(const YAML::Node& at, Resource resource);
    void addEnvironment(ParseCtx& ctx,
                        const YAML::Node& at,
                        const std::string& name,
                        std::map<std::string, std::string> vars);
    void loadEnvironmentFile(ParseCtx& ctx, const YAML::Node& doc, const fs::path& file);
    void readInlineEntities(ParseCtx& ctx, const YAML::Node& root);

    fs::path rootYaml_;
    fs::path baseDir_;
    Project project_;
};

void ProjectLoader::addActor(const YAML::Node& at, Actor actor) {
    if (const auto it = project_.actors.find(actor.id); it != project_.actors.end()) {
        fault(at,
              std::format("actor '{}' is already defined in {}:{}",
                          actor.id.value,
                          it->second.source.file,
                          it->second.source.line));
    }
    project_.actors[actor.id] = std::move(actor);
}

void ProjectLoader::addResource(const YAML::Node& at, Resource resource) {
    if (const auto it = project_.resources.find(resource.id); it != project_.resources.end()) {
        fault(at,
              std::format("resource '{}' is already defined in {}:{}",
                          resource.id.value,
                          it->second.source.file,
                          it->second.source.line));
    }
    project_.resources[resource.id] = std::move(resource);
}

void ProjectLoader::addEnvironment(ParseCtx& ctx,
                                   const YAML::Node& at,
                                   const std::string& name,
                                   std::map<std::string, std::string> vars) {
    if (const auto it = project_.environmentSources.find(name);
        it != project_.environmentSources.end()) {
        fault(at,
              std::format("environment '{}' is already defined in {}:{}",
                          name,
                          it->second.file,
                          it->second.line));
    }
    project_.environments[name] = std::move(vars);
    project_.environmentSources[name] = SourceLocation{ctx.file, at.Mark().line + 1};
}

void ProjectLoader::loadEnvironmentFile(ParseCtx& ctx,
                                        const YAML::Node& doc,
                                        const fs::path& file) {
    // Wrapped: `name:` + `variables:` (+ `transport:`). Flat: every other scalar key is a var.
    if (!doc.IsMap()) {
        fault(doc, "an environment file must be a map, got " + describe(doc));
    }
    const auto envName = readString(doc, "name", file.stem().string());
    std::map<std::string, std::string> vars;
    if (const auto variables = mapAt(doc, "variables"); present(variables)) {
        warnUnknownKeys(ctx, doc, {"name", "variables", "transport"}, "environment file");
        for (const auto& kv : variables) {
            const auto key = kv.first.as<std::string>();
            vars[key] = readEnvScalar(key, kv.second);
        }
    } else {
        for (const auto& kv : doc) {
            const auto key = kv.first.as<std::string>();
            if (key != "name" && key != "transport" && key != "variables") {
                vars[key] = readEnvScalar(key, kv.second);
            }
        }
    }
    addEnvironment(ctx, doc, envName, std::move(vars));
    if (present(doc["transport"])) {
        project_.transport[envName] = parseTransport(ctx, doc);
    }
}

std::optional<ReqloomError> ProjectLoader::loadSubFile(const fs::path& file, SubFileKind kind) {
    const auto key = canonicalSchemaPath(file);
    if (project_.loadedFiles.contains(key)) {
        return std::nullopt;
    }
    auto loaded = loadYamlCapped(file);
    if (!loaded) {
        return loaded.error();
    }
    project_.loadedFiles[key] = loaded->hash;
    auto ctx = contextFor(file);
    const YAML::Node doc = loaded->root;
    return guarded(ctx.file, [&] {
        if (!present(doc)) {
            ctx.warn(YAML::Mark::null_mark(), "file is empty; ignored");
            return;
        }
        if (kind == SubFileKind::Actor) {
            auto entity = unwrapEntity(doc, file, kActorFields);
            addActor(entity.body, parseActor(ctx, entity.id, entity.body, entity.line));
        } else if (kind == SubFileKind::Resource) {
            auto entity = unwrapEntity(doc, file, kResourceFields);
            addResource(entity.body,
                        parseResource(ctx, entity.id, entity.body, entity.line, baseDir_));
        } else {
            loadEnvironmentFile(ctx, doc, file);
        }
    });
}

std::optional<ReqloomError> ProjectLoader::processImports(ParseCtx& ctx, const YAML::Node& root) {
    std::vector<std::pair<std::string, YAML::Node>> patterns;
    if (auto err = guarded(ctx.file, [&] {
            const auto importsNode = root["imports"];
            if (!present(importsNode)) {
                return;
            }
            const auto addPattern = [&](const YAML::Node& p) {
                if (!p.IsScalar()) {
                    fault(p, "an imports entry must be a path pattern, got " + describe(p));
                }
                patterns.emplace_back(p.Scalar(), p);
            };
            if (importsNode.IsSequence()) {
                for (const auto& p : importsNode) {
                    addPattern(p);
                }
            } else if (importsNode.IsMap()) {
                for (const auto& kv : importsNode) {
                    if (kv.second.IsSequence()) {
                        for (const auto& p : kv.second) {
                            addPattern(p);
                        }
                    } else {
                        addPattern(kv.second);
                    }
                }
            } else {
                fault(importsNode,
                      "'imports' must be a list of path patterns, got " + describe(importsNode));
            }
        })) {
        return err;
    }

    for (const auto& [pattern, at] : patterns) {
        auto files = resolveGlob(baseDir_, pattern);
        if (!files) {
            return files.error();
        }
        if (files->empty()) {
            ctx.warn(at.Mark(), std::format("imports pattern '{}' matched no files", pattern));
        }
        for (const auto& file : *files) {
            std::error_code ec;
            const auto relPath = fs::relative(file, baseDir_, ec).generic_string();
            const auto kind = ec ? SubFileKind::Other : subFileKind(relPath);
            if (kind == SubFileKind::Other) {
                ctx.warn(at.Mark(),
                         std::format("imported file {} is not under actors/, resources/ or "
                                     "environments/; ignored",
                                     file.string()));
                continue;
            }
            if (auto err = loadSubFile(file, kind)) {
                return err;
            }
        }
    }
    return std::nullopt;
}

void ProjectLoader::readRootSettings(ParseCtx& ctx, const YAML::Node& root) {
    warnUnknownKeys(ctx, root, kRootKeys, "reqloom.yaml");
    project_.name = readString(root, "name", "Unnamed Project");
    project_.defaultEnvironment = readString(root, "default_environment", "local");
    if (const auto id = readOptString(root, "id")) {
        project_.id = *id;
        if (auto ok = validateProjectId(project_.id); !ok) {
            fault(root["id"], ok.error().detail);
        }
    }
    if (const auto allowedEnv = root["allowed_env"]; allowedEnv) {
        if (!allowedEnv.IsSequence()) {
            fault(allowedEnv, "allowed_env must be a list of environment variable names");
        }
        for (const auto& item : allowedEnv) {
            if (!item.IsScalar()) {
                fault(item, "an allowed_env item must be a variable name, got " + describe(item));
            }
            project_.allowedEnv.push_back(item.Scalar());
        }
    }
    // Project-wide default auth, the target of an operation's `type: inherit`.
    project_.defaultAuth = parseInlineAuth(ctx, root);
    // latency_slo: { p95_ms: 800 }; negative or missing means unset (0).
    if (const auto slo = mapAt(root, "latency_slo"); present(slo)) {
        warnUnknownKeys(ctx, slo, {"p95_ms"}, "latency_slo");
        project_.latencySloP95Ms = std::max(0, readInt(slo, "p95_ms", 0));
    }
    project_.failureMessageShow =
        parseFailureMessage(ctx, root).value_or(std::vector<std::string>{});
}

void ProjectLoader::readInlineEntities(ParseCtx& ctx, const YAML::Node& root) {
    if (const auto actors = mapAt(root, "actors"); present(actors)) {
        for (const auto& kv : actors) {
            const auto actorId = kv.first.as<std::string>();
            addActor(kv.first, parseActor(ctx, actorId, kv.second, kv.first.Mark().line + 1));
        }
    }
    if (const auto resources = mapAt(root, "resources"); present(resources)) {
        for (const auto& kv : resources) {
            const auto resourceId = kv.first.as<std::string>();
            addResource(
                kv.first,
                parseResource(ctx, resourceId, kv.second, kv.first.Mark().line + 1, baseDir_));
        }
    }
    if (const auto environment = mapAt(root, "environment"); present(environment)) {
        std::map<std::string, std::string> vars;
        for (const auto& kv : environment) {
            const auto key = kv.first.as<std::string>();
            vars[key] = readEnvScalar(key, kv.second);
        }
        addEnvironment(ctx, environment, project_.defaultEnvironment, std::move(vars));
    }
    // Root-level `transport:` applies to the default environment only.
    if (const auto transport = root["transport"]; present(transport)) {
        if (project_.transport.contains(project_.defaultEnvironment)) {
            fault(transport,
                  std::format("transport for environment '{}' is already set in its environment "
                              "file",
                              project_.defaultEnvironment));
        }
        project_.transport[project_.defaultEnvironment] = parseTransport(ctx, root);
    }
}

std::expected<Project, ReqloomError> ProjectLoader::load() {
    auto loadedRoot = loadYamlCapped(rootYaml_);
    if (!loadedRoot) {
        return std::unexpected(loadedRoot.error());
    }
    project_.loadedFiles[canonicalSchemaPath(rootYaml_)] = loadedRoot->hash;
    const YAML::Node root = loadedRoot->root;
    auto ctx = contextFor(rootYaml_);

    std::optional<ReqloomError> versionError;
    if (auto err = guarded(ctx.file, [&] {
            if (present(root) && !root.IsMap()) {
                fault(root, "reqloom.yaml must be a map, got " + describe(root));
            }
            const auto version = readInt(root, "version", 0);
            if (version < 1 || version > 3) {
                versionError =
                    schemaError(ErrorCode::SchemaVersion,
                                "Unsupported schema version " + std::to_string(version) +
                                    " (supported: 1–3). Run `reqloom migrate` to upgrade.");
                return;
            }
            readRootSettings(ctx, root);
        })) {
        return std::unexpected(*err);
    }
    if (versionError) {
        return std::unexpected(*versionError);
    }
    if (auto err = processImports(ctx, root)) {
        return std::unexpected(*err);
    }
    if (auto err = guarded(ctx.file, [&] { readInlineEntities(ctx, root); })) {
        return std::unexpected(*err);
    }

    // Reject undefined references, missing depends_on targets and cycles at load.
    if (auto valid = DependencyResolver{}.validate(project_); !valid) {
        return std::unexpected(valid.error());
    }
    return std::move(project_);
}

}  // namespace

YamlSchemaParser::YamlSchemaParser() = default;
YamlSchemaParser::~YamlSchemaParser() = default;

SchemaParseResult YamlSchemaParser::parse(const fs::path& rootYaml) {
    std::error_code ec;
    if (!fs::exists(rootYaml, ec) || ec) {
        return std::unexpected(ReqloomError{
            ErrorCode::YamlParse, ErrorClass::Schema, "File not found: " + rootYaml.string()});
    }
    return ProjectLoader{rootYaml}.load();
}

}  // namespace reqloom::engine
