// YamlSchemaWriter — writes Project to reqloom.yaml.
#include "YamlSchemaWriter.h"

#include "../../domain/Codecs.h"
#include "../../domain/DependencyResolver.h"
#include "SchemaParser.h"

#include <reqloom/engine/ProjectIdentity.h>

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <random>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace reqloom::engine {

namespace {

using codecs::methodToString;

// Note: only JsonPath round-trips through the parser today (header-style is detected by the
// `$.headers.` prefix); XPath / Regex / Cookie / StatusCode are emit-only stubs.

constexpr std::string_view provenanceSourceToString(Provenance::Source s) {
    switch (s) {
        case Provenance::Source::HandWritten:
            return "hand_written";
        case Provenance::Source::OpenApiImport:
            return "openapi_import";
        case Provenance::Source::PostmanImport:
            return "postman_import";
        case Provenance::Source::BrunoImport:
            return "bruno_import";
        case Provenance::Source::InsomniaImport:
            return "insomnia_import";
        case Provenance::Source::HarImport:
            return "har_import";
        case Provenance::Source::AiImport:
            return "ai_import";
        case Provenance::Source::ApidogImport:
            return "apidog_import";
        case Provenance::Source::HoppscotchImport:
            return "hoppscotch_import";
        case Provenance::Source::ThunderClientImport:
            return "thunder_client_import";
        case Provenance::Source::HttpieImport:
            return "httpie_import";
        case Provenance::Source::HttpFileImport:
            return "http_file_import";
        case Provenance::Source::Unknown:
            return "unknown";
    }
    return "hand_written";
}

constexpr std::string_view inlineAuthTypeToString(InlineAuthType t) {
    switch (t) {
        case InlineAuthType::None:
            return "none";
        case InlineAuthType::Bearer:
            return "bearer";
        case InlineAuthType::Basic:
            return "basic";
        case InlineAuthType::ApiKey:
            return "apikey";
        case InlineAuthType::AwsSigV4:
            return "aws_sigv4";
        case InlineAuthType::OAuth1:
            return "oauth1";
        case InlineAuthType::OAuth2:
            return "oauth2";
        case InlineAuthType::Jwt:
            return "jwt";
        case InlineAuthType::Mtls:
            return "mtls";
        case InlineAuthType::Inherit:
            return "inherit";
    }
    return "none";
}

constexpr std::string_view verifiedAgainstToString(Provenance::VerifiedAgainst v) {
    switch (v) {
        case Provenance::VerifiedAgainst::None:
            return "none";
        case Provenance::VerifiedAgainst::OpenApiExample:
            return "openapi_example";
        case Provenance::VerifiedAgainst::PostmanResponse:
            return "postman_response";
        case Provenance::VerifiedAgainst::InsomniaResponse:
            return "insomnia_response";
        case Provenance::VerifiedAgainst::HarEntry:
            return "har_entry";
        case Provenance::VerifiedAgainst::Synthetic:
            return "synthetic";
        case Provenance::VerifiedAgainst::LiveCapture:
            return "live_capture";
    }
    return "none";
}

// True when `target` already holds `content`, so editing one operation does not rewrite other
// files.
[[nodiscard]] bool fileHasContent(const fs::path& target, const std::string& content) {
    std::error_code ec;
    // A directory opens as a stream on Linux and then throws on the first read.
    if (!fs::is_regular_file(target, ec) || ec) {
        return false;
    }
    std::ifstream in{target, std::ios::binary};
    if (!in) {
        return false;
    }
    const std::string existing{std::istreambuf_iterator<char>(in),
                               std::istreambuf_iterator<char>()};
    return existing == content;
}

[[nodiscard]] fs::path uniqueTempPath(const fs::path& target) {
#ifdef _WIN32
    const auto pid = _getpid();
#else
    const auto pid = getpid();
#endif
    thread_local std::mt19937_64 rng{std::random_device{}()};
    auto temp = target;
    temp += std::format(".{}-{:016x}.tmp", pid, rng());
    return temp;
}

// Write `content` to a uniquely named temp sibling of `target`; the caller commits by renaming.
// Two-phase staging makes a multi-file save near-atomic.
[[nodiscard]] std::expected<fs::path, ReqloomError> stageTemp(const fs::path& target,
                                                              const std::string& content) {
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    if (ec) {
        return std::unexpected(ReqloomError{
            ErrorCode::SchemaInvalid,
            ErrorClass::Schema,
            "writer: cannot create dir " + target.parent_path().string() + ": " + ec.message()});
    }
    const auto temp = uniqueTempPath(target);
    std::ofstream out{temp, std::ios::binary | std::ios::trunc};
    if (!out) {
        return std::unexpected(ReqloomError{ErrorCode::SchemaInvalid,
                                            ErrorClass::Schema,
                                            "writer: cannot open temp file " + temp.string()});
    }
    out << content;
    out.close();
    if (!out) {
        std::error_code rmEc;
        fs::remove(temp, rmEc);
        return std::unexpected(ReqloomError{ErrorCode::SchemaInvalid,
                                            ErrorClass::Schema,
                                            "writer: failed writing temp file " + temp.string()});
    }
    return temp;
}

[[nodiscard]] bool isUnder(const fs::path& root, const fs::path& path) {
    std::error_code baseEc;
    std::error_code fullEc;
    const auto base = fs::absolute(root, baseEc).lexically_normal();
    const auto full = fs::absolute(path, fullEc).lexically_normal();
    if (baseEc || fullEc) {
        return false;
    }
    const auto rel = full.lexically_relative(base);
    return !rel.empty() && rel.is_relative() && *rel.begin() != ".." && *rel.begin() != ".";
}

[[nodiscard]] std::expected<void, ReqloomError> checkStem(std::string_view kind,
                                                          const std::string& stem) {
    auto ok = checkEntityFileStem(kind, stem);
    if (!ok) {
        auto error = ok.error();
        error.detail = "writer: " + error.detail;
        return std::unexpected(std::move(error));
    }
    return {};
}

using SavePlan = std::vector<std::pair<fs::path, std::string>>;

[[nodiscard]] std::optional<std::string> readFileBytes(const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        return std::nullopt;
    }
    std::ifstream in{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

[[nodiscard]] ReqloomError writerError(std::string detail) {
    return ReqloomError{ErrorCode::SchemaInvalid, ErrorClass::Schema, std::move(detail)};
}

[[nodiscard]] ReqloomError changedOnDisk(const fs::path& path) {
    return writerError(
        std::format("writer: {} changed on disk since the project was loaded; reload before saving",
                    path.string()));
}

/// Refuse the save when a target or a prune candidate is not what this model loaded. Returns
/// the loaded files to prune: those in the managed folders that the plan no longer writes.
[[nodiscard]] std::expected<std::vector<fs::path>, ReqloomError> checkAgainstLoaded(
    const fs::path& targetDir,
    const SavePlan& plan,
    const std::map<fs::path, std::size_t>& loaded,
    bool prune) {
    std::set<fs::path> planKeys;
    for (const auto& [target, content] : plan) {
        const auto key = canonicalSchemaPath(target);
        planKeys.insert(key);
        const auto onDisk = readFileBytes(target);
        if (!onDisk || *onDisk == content) {
            continue;
        }
        const auto it = loaded.find(key);
        if (it == loaded.end()) {
            return std::unexpected(writerError(std::format(
                "writer: {} exists but was not loaded with this project (another program may "
                "have created it); reload before saving",
                target.string())));
        }
        if (schemaFileHash(*onDisk) != it->second) {
            return std::unexpected(changedOnDisk(target));
        }
    }

    std::vector<fs::path> toPrune;
    if (!prune) {
        return toPrune;
    }
    const std::set<fs::path> managed{canonicalSchemaPath(targetDir / "actors"),
                                     canonicalSchemaPath(targetDir / "resources"),
                                     canonicalSchemaPath(targetDir / "environments")};
    for (const auto& [path, hash] : loaded) {
        if (planKeys.contains(path) || !managed.contains(path.parent_path())) {
            continue;
        }
        const auto onDisk = readFileBytes(path);
        if (!onDisk) {
            continue;
        }
        if (schemaFileHash(*onDisk) != hash) {
            return std::unexpected(changedOnDisk(path));
        }
        toPrune.push_back(path);
    }
    return toPrune;
}

void finishGuardedSave(const SavePlan& plan,
                       const std::vector<fs::path>& toPrune,
                       std::map<fs::path, std::size_t>& loaded) {
    for (const auto& path : toPrune) {
        std::error_code rmEc;
        fs::remove(path, rmEc);
        if (!rmEc) {
            loaded.erase(path);
        }
    }
    for (const auto& [target, content] : plan) {
        loaded[canonicalSchemaPath(target)] = schemaFileHash(content);
    }
}

// Remove stale `*.yaml` files in a managed sub-directory whose stem is no longer a current entity,
// so a rename or delete does not reload as a ghost entity. Other files are left alone.
void pruneStaleFiles(const fs::path& root,
                     const fs::path& dir,
                     const std::set<std::string>& keepStems) {
    std::error_code ec;
    if (!isUnder(root, dir) || !fs::is_directory(dir, ec)) {
        return;
    }
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) {
            break;
        }
        const auto& path = entry.path();
        if (!entry.is_regular_file() || path.extension() != ".yaml" || !isUnder(dir, path)) {
            continue;
        }
        if (!keepStems.contains(path.stem().string())) {
            std::error_code rmEc;
            fs::remove(path, rmEc);
        }
    }
}

void emitStringMap(YAML::Emitter& e, const std::map<std::string, std::string>& m) {
    e << YAML::BeginMap;
    for (const auto& [k, v] : m) {
        e << YAML::Key << k << YAML::Value << v;
    }
    e << YAML::EndMap;
}

// Emit a request `body`. The parser stores scalar bodies verbatim and never re-encodes them, so
// emitting the stored body as one scalar round-trips exactly and repeated saves are idempotent. A
// multi-line body without a trailing newline uses a block literal (`|-`), which round-trips the
// exact bytes; one ending in a newline stays on the scalar path.
void emitBodyTemplate(YAML::Emitter& e, const std::string& bodyTemplate) {
    if (bodyTemplate.find('\n') != std::string::npos && !bodyTemplate.empty() &&
        bodyTemplate.back() != '\n') {
        e << YAML::Literal << bodyTemplate;
    } else {
        e << bodyTemplate;
    }
}

constexpr std::string_view extractionSourceToString(Extraction::Source s) {
    switch (s) {
        case Extraction::Source::JsonPath:
            return "jsonpath";
        case Extraction::Source::XPath:
            return "xpath";
        case Extraction::Source::Header:
            return "header";
        case Extraction::Source::StatusCode:
            return "status_code";
        case Extraction::Source::Regex:
            return "regex";
        case Extraction::Source::Cookie:
            return "cookie";
    }
    return "jsonpath";
}

// The source the parser would auto-detect from a path prefix; when it matches, the compact scalar
// form is safe, otherwise the explicit map form preserves the source.
constexpr Extraction::Source autoDetectExtractionSource(std::string_view path) {
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

void emitExtractions(YAML::Emitter& e, const std::vector<Extraction>& extractions) {
    if (extractions.empty()) {
        return;
    }
    e << YAML::Key << "extract" << YAML::Value << YAML::BeginMap;
    for (const auto& ext : extractions) {
        // Compact `var: path` when the source is derivable from the path, else the explicit map
        // form.
        if (ext.source == autoDetectExtractionSource(ext.sourcePath)) {
            e << YAML::Key << ext.variableName << YAML::Value << ext.sourcePath;
        } else {
            e << YAML::Key << ext.variableName << YAML::Value << YAML::BeginMap << YAML::Key
              << "path" << YAML::Value << ext.sourcePath << YAML::Key << "source" << YAML::Value
              << std::string{extractionSourceToString(ext.source)} << YAML::EndMap;
        }
    }
    e << YAML::EndMap;
}

void emitAssertions(YAML::Emitter& e, const std::vector<Assertion>& assertions) {
    if (assertions.empty()) {
        return;
    }
    e << YAML::Key << "assert" << YAML::Value << YAML::BeginSeq;
    for (const auto& assertion : assertions) {
        // Compact scalar when unnamed, `{ expr, name }` map when labelled.
        if (assertion.name) {
            e << YAML::BeginMap << YAML::Key << "expr" << YAML::Value << assertion.expr << YAML::Key
              << "name" << YAML::Value << *assertion.name << YAML::EndMap;
        } else {
            e << assertion.expr;
        }
    }
    e << YAML::EndSeq;
}

void emitFailureMessage(YAML::Emitter& e, const std::vector<std::string>& show) {
    e << YAML::Key << "failure_message" << YAML::Value << YAML::BeginMap << YAML::Key << "show"
      << YAML::Value << YAML::Flow << YAML::BeginSeq;
    for (const auto& path : show) {
        e << path;
    }
    e << YAML::EndSeq << YAML::EndMap;
}

void emitProvenance(YAML::Emitter& e, const Provenance& p) {
    e << YAML::Key << "_provenance" << YAML::Value << YAML::BeginMap;
    e << YAML::Key << "source" << YAML::Value
      << (p.source == Provenance::Source::Unknown && !p.unknownSource.empty()
              ? p.unknownSource
              : std::string{provenanceSourceToString(p.source)});
    if (p.verifiedAgainst != Provenance::VerifiedAgainst::None) {
        e << YAML::Key << "verified_against" << YAML::Value
          << std::string{verifiedAgainstToString(p.verifiedAgainst)};
    }
    if (p.model) {
        e << YAML::Key << "model" << YAML::Value << *p.model;
    }
    if (p.importedAt) {
        e << YAML::Key << "imported_at" << YAML::Value << *p.importedAt;
    }
    if (!p.evidence.empty()) {
        e << YAML::Key << "evidence" << YAML::Value;
        emitStringMap(e, p.evidence);
    }
    e << YAML::EndMap;
}

void emitInlineAuth(YAML::Emitter& e, const InlineAuth& a) {
    e << YAML::Key << "auth" << YAML::Value << YAML::BeginMap;
    e << YAML::Key << "type" << YAML::Value << std::string{inlineAuthTypeToString(a.type)};
    switch (a.type) {
        case InlineAuthType::Bearer:
            e << YAML::Key << "token" << YAML::Value << a.token;
            break;
        case InlineAuthType::Basic:
            e << YAML::Key << "username" << YAML::Value << a.username;
            e << YAML::Key << "password" << YAML::Value << a.password;
            break;
        case InlineAuthType::ApiKey:
            e << YAML::Key << "key" << YAML::Value << a.apiKeyName;
            e << YAML::Key << "value" << YAML::Value << a.apiKeyValue;
            e << YAML::Key << "in" << YAML::Value
              << std::string{a.apiKeyInQuery ? "query" : "header"};
            break;
        case InlineAuthType::AwsSigV4:
            e << YAML::Key << "access_key" << YAML::Value << a.awsAccessKey;
            e << YAML::Key << "secret_key" << YAML::Value << a.awsSecretKey;
            e << YAML::Key << "region" << YAML::Value << a.awsRegion;
            e << YAML::Key << "service" << YAML::Value << a.awsService;
            if (!a.awsSessionToken.empty()) {
                e << YAML::Key << "session_token" << YAML::Value << a.awsSessionToken;
            }
            break;
        case InlineAuthType::OAuth1:
            e << YAML::Key << "consumer_key" << YAML::Value << a.oauthConsumerKey;
            e << YAML::Key << "consumer_secret" << YAML::Value << a.oauthConsumerSecret;
            e << YAML::Key << "oauth_token" << YAML::Value << a.oauthToken;
            e << YAML::Key << "token_secret" << YAML::Value << a.oauthTokenSecret;
            break;
        case InlineAuthType::OAuth2:
            e << YAML::Key << "grant_type" << YAML::Value
              << (a.oauth2GrantType.empty() ? std::string{"client_credentials"}
                                            : a.oauth2GrantType);
            e << YAML::Key << "token_url" << YAML::Value << a.oauth2TokenUrl;
            e << YAML::Key << "client_id" << YAML::Value << a.oauth2ClientId;
            e << YAML::Key << "client_secret" << YAML::Value << a.oauth2ClientSecret;
            if (a.oauth2GrantType == "password") {
                e << YAML::Key << "username" << YAML::Value << a.username;
                e << YAML::Key << "password" << YAML::Value << a.password;
            }
            if (a.oauth2GrantType == "authorization_code") {
                e << YAML::Key << "auth_url" << YAML::Value << a.oauth2AuthUrl;
                e << YAML::Key << "callback_url" << YAML::Value << a.oauth2CallbackUrl;
                if (!a.oauth2PkceMethod.empty()) {
                    e << YAML::Key << "pkce_method" << YAML::Value << a.oauth2PkceMethod;
                }
                // oauth2AccessToken is deliberately not emitted — ephemeral secret.
            }
            if (!a.oauth2Scope.empty()) {
                e << YAML::Key << "scope" << YAML::Value << a.oauth2Scope;
            }
            if (!a.oauth2ClientAuth.empty()) {
                e << YAML::Key << "client_auth" << YAML::Value << a.oauth2ClientAuth;
            }
            break;
        case InlineAuthType::Jwt:
            e << YAML::Key << "algorithm" << YAML::Value
              << (a.jwtAlgorithm.empty() ? std::string{"HS256"} : a.jwtAlgorithm);
            e << YAML::Key << "secret" << YAML::Value << a.jwtSecret;
            e << YAML::Key << "payload" << YAML::Value << a.jwtPayload;
            break;
        case InlineAuthType::Mtls:
            e << YAML::Key << "format" << YAML::Value
              << (a.mtlsFormat.empty() ? std::string{"pem"} : a.mtlsFormat);
            e << YAML::Key << "cert_path" << YAML::Value << a.mtlsCertPath;
            if (a.mtlsFormat != "p12") {
                e << YAML::Key << "key_path" << YAML::Value << a.mtlsKeyPath;
            }
            if (!a.mtlsKeyPassword.empty()) {
                e << YAML::Key << "key_password" << YAML::Value << a.mtlsKeyPassword;
            }
            if (!a.mtlsCaCertPath.empty()) {
                e << YAML::Key << "ca_cert_path" << YAML::Value << a.mtlsCaCertPath;
            }
            break;
        case InlineAuthType::Inherit:
        case InlineAuthType::None:
            break;
    }
    e << YAML::EndMap;
}

void emitOperation(YAML::Emitter& e, const Operation& op) {
    e << YAML::BeginMap;
    e << YAML::Key << "method" << YAML::Value << std::string{methodToString(op.method)};
    e << YAML::Key << "path" << YAML::Value << op.pathTemplate;
    if (!op.actor.value.empty()) {
        e << YAML::Key << "actor" << YAML::Value << op.actor.value;
    }
    if (op.inlineAuth && op.inlineAuth->type != InlineAuthType::None) {
        emitInlineAuth(e, *op.inlineAuth);
    }
    if (!op.headers.empty()) {
        e << YAML::Key << "headers" << YAML::Value;
        emitStringMap(e, op.headers);
    }
    if (!op.queryParams.empty()) {
        e << YAML::Key << "query_params" << YAML::Value;
        emitStringMap(e, op.queryParams);
    }
    if (op.bodyTemplate) {
        e << YAML::Key << "body" << YAML::Value;
        emitBodyTemplate(e, *op.bodyTemplate);
    }
    if (op.bodyForm) {
        e << YAML::Key << "body_form" << YAML::Value;
        emitStringMap(e, *op.bodyForm);
    }
    if (!op.expectStatusList.empty()) {
        e << YAML::Key << "expect_status" << YAML::Value << YAML::Flow << YAML::BeginSeq;
        for (int const s : op.expectStatusList) {
            e << s;
        }
        e << YAML::EndSeq;
    } else if (op.expectStatus) {
        e << YAML::Key << "expect_status" << YAML::Value << *op.expectStatus;
    }
    if (!op.explicitDependencies.empty()) {
        e << YAML::Key << "depends_on" << YAML::Value << YAML::Flow << YAML::BeginSeq;
        for (const auto& dep : op.explicitDependencies) {
            e << dep.value;
        }
        e << YAML::EndSeq;
    }
    if (op.preRequestScriptRef) {
        e << YAML::Key << "pre_request" << YAML::Value << *op.preRequestScriptRef;
    } else if (op.preRequestScript) {
        e << YAML::Key << "pre_request" << YAML::Value << YAML::Literal << *op.preRequestScript;
    }
    if (op.postResponseScriptRef) {
        e << YAML::Key << "post_response" << YAML::Value << *op.postResponseScriptRef;
    } else if (op.postResponseScript) {
        e << YAML::Key << "post_response" << YAML::Value << YAML::Literal << *op.postResponseScript;
    }
    if (op.timeout) {
        e << YAML::Key << "timeout" << YAML::Value << static_cast<int>(op.timeout->count());
    }
    if (op.force) {
        e << YAML::Key << "force" << YAML::Value << true;
    }
    // Emit `retry` only when it differs from the defaults (max 3, backoff 500ms, non_idempotent
    // false); `max` and `backoff` go together, `non_idempotent` only when true.
    if (op.retry.maxAttempts != RetryPolicy{}.maxAttempts ||
        op.retry.baseBackoff != RetryPolicy{}.baseBackoff || op.retry.nonIdempotent) {
        e << YAML::Key << "retry" << YAML::Value << YAML::BeginMap << YAML::Key << "max"
          << YAML::Value << op.retry.maxAttempts << YAML::Key << "backoff" << YAML::Value
          << static_cast<int>(op.retry.baseBackoff.count());
        if (op.retry.nonIdempotent) {
            e << YAML::Key << "non_idempotent" << YAML::Value << true;
        }
        e << YAML::EndMap;
    }
    if (op.pollUntil) {
        const auto& p = *op.pollUntil;
        e << YAML::Key << "poll_until" << YAML::Value << YAML::BeginMap << YAML::Key << "method"
          << YAML::Value << std::string{methodToString(p.method)} << YAML::Key << "path"
          << YAML::Value << p.pathTemplate << YAML::Key << "success_when" << YAML::Value
          << p.successWhen;
        if (p.actor) {
            e << YAML::Key << "actor" << YAML::Value << p.actor->value;
        }
        if (p.failWhen) {
            e << YAML::Key << "fail_when" << YAML::Value << *p.failWhen;
        }
        if (p.backoffBase) {
            e << YAML::Key << "backoff" << YAML::Value << YAML::BeginMap << YAML::Key << "base"
              << YAML::Value << (std::to_string(p.backoffBase->count()) + "ms") << YAML::Key
              << "max" << YAML::Value << (std::to_string(p.backoffMax.count()) + "ms")
              << YAML::EndMap;
        } else {
            e << YAML::Key << "interval" << YAML::Value
              << (std::to_string(p.interval.count()) + "ms");
        }
        e << YAML::Key << "timeout" << YAML::Value << (std::to_string(p.timeout.count()) + "ms")
          << YAML::Key << "max_attempts" << YAML::Value << p.maxAttempts << YAML::EndMap;
    }
    if (op.forEach) {
        e << YAML::Key << "for_each" << YAML::Value << YAML::BeginMap << YAML::Key << "over"
          << YAML::Value << op.forEach->over.value;
        if (op.forEach->continueOnError) {
            e << YAML::Key << "continue_on_error" << YAML::Value << true;
        }
        e << YAML::EndMap;
    }
    emitExtractions(e, op.extractions);
    emitAssertions(e, op.assertions);
    if (op.failureMessageShow) {
        emitFailureMessage(e, *op.failureMessageShow);
    }
    if (op.provenance) {
        emitProvenance(e, *op.provenance);
    }
    e << YAML::EndMap;
}

std::string emitResource(const Resource& res) {
    YAML::Emitter e;
    e << YAML::BeginMap << YAML::Key << "name" << YAML::Value << res.id.value << YAML::Key
      << "description" << YAML::Value << res.description << YAML::Key << "operations" << YAML::Value
      << YAML::BeginMap;
    std::vector<std::string> opNames;
    opNames.reserve(res.operations.size());
    for (const auto& [k, _] : res.operations) {
        opNames.push_back(k);
    }
    std::sort(opNames.begin(), opNames.end());
    for (const auto& name : opNames) {
        e << YAML::Key << name << YAML::Value;
        emitOperation(e, res.operations.at(name));
    }
    e << YAML::EndMap << YAML::EndMap;
    return e.c_str();
}

void emitAuthStep(YAML::Emitter& e, const AuthStep& step, bool isChainStep) {
    if (isChainStep) {
        e << YAML::BeginMap;
        e << YAML::Key << "id" << YAML::Value << step.id;
    }
    e << YAML::Key << "method" << YAML::Value << std::string{methodToString(step.method)}
      << YAML::Key << "path" << YAML::Value << step.pathTemplate;
    if (!step.headers.empty()) {
        e << YAML::Key << "headers" << YAML::Value;
        emitStringMap(e, step.headers);
    }
    if (step.bodyTemplate) {
        e << YAML::Key << "body" << YAML::Value;
        emitBodyTemplate(e, *step.bodyTemplate);
    }
    if (step.expectStatus) {
        e << YAML::Key << "expect_status" << YAML::Value << *step.expectStatus;
    }
    emitExtractions(e, step.extractions);
    if (isChainStep) {
        e << YAML::EndMap;
    }
}

std::string emitActor(const Actor& actor) {
    YAML::Emitter e;
    e << YAML::BeginMap << YAML::Key << "name" << YAML::Value << actor.id.value << YAML::Key
      << "description" << YAML::Value << actor.description;

    e << YAML::Key << "auth" << YAML::Value << YAML::BeginMap;
    if (actor.strategy == AuthStrategy::Chain) {
        e << YAML::Key << "strategy" << YAML::Value << "chain";
        e << YAML::Key << "steps" << YAML::Value << YAML::BeginSeq;
        for (const auto& step : actor.authSteps) {
            emitAuthStep(e, step, /*isChainStep=*/true);
        }
        e << YAML::EndSeq;
    } else if (actor.strategy == AuthStrategy::Basic) {
        e << YAML::Key << "strategy" << YAML::Value << "basic";
        if (auto it = actor.authConfig.find("username"); it != actor.authConfig.end()) {
            e << YAML::Key << "username" << YAML::Value << it->second;
        }
        if (auto it = actor.authConfig.find("password"); it != actor.authConfig.end()) {
            e << YAML::Key << "password" << YAML::Value << it->second;
        }
    } else if (actor.strategy == AuthStrategy::ApiKey) {
        e << YAML::Key << "strategy" << YAML::Value << "api_key";
        if (auto it = actor.authConfig.find("key"); it != actor.authConfig.end()) {
            e << YAML::Key << "key" << YAML::Value << it->second;
        }
        if (auto it = actor.authConfig.find("location"); it != actor.authConfig.end()) {
            e << YAML::Key << "location" << YAML::Value << it->second;
        }
        if (auto it = actor.authConfig.find("name"); it != actor.authConfig.end()) {
            e << YAML::Key << "name" << YAML::Value << it->second;
        }
    } else if (actor.strategy == AuthStrategy::OAuth2ClientCredentials) {
        e << YAML::Key << "strategy" << YAML::Value << "oauth2_client_credentials";
        for (const auto* field : {"token_url", "client_id", "client_secret", "scope"}) {
            if (auto it = actor.authConfig.find(field); it != actor.authConfig.end()) {
                e << YAML::Key << field << YAML::Value << it->second;
            }
        }
    } else if (actor.strategy == AuthStrategy::OAuth2Password) {
        e << YAML::Key << "strategy" << YAML::Value << "oauth2_password";
        for (const auto* field :
             {"token_url", "client_id", "client_secret", "username", "password", "scope"}) {
            if (auto it = actor.authConfig.find(field); it != actor.authConfig.end()) {
                e << YAML::Key << field << YAML::Value << it->second;
            }
        }
    } else if (actor.strategy == AuthStrategy::OAuth1) {
        e << YAML::Key << "strategy" << YAML::Value << "oauth1";
        for (const auto* field :
             {"consumer_key", "consumer_secret", "token", "token_secret", "realm"}) {
            if (auto it = actor.authConfig.find(field); it != actor.authConfig.end()) {
                e << YAML::Key << field << YAML::Value << it->second;
            }
        }
    } else if (actor.strategy == AuthStrategy::AwsSigV4) {
        e << YAML::Key << "strategy" << YAML::Value << "aws_sigv4";
        for (const auto* field :
             {"access_key", "secret_key", "region", "service", "session_token"}) {
            if (auto it = actor.authConfig.find(field); it != actor.authConfig.end()) {
                e << YAML::Key << field << YAML::Value << it->second;
            }
        }
    } else if (actor.strategy == AuthStrategy::Bearer) {
        e << YAML::Key << "strategy" << YAML::Value << "bearer";
        if (auto it = actor.authConfig.find("token"); it != actor.authConfig.end()) {
            e << YAML::Key << "token" << YAML::Value << it->second;
        }
    } else if (actor.strategy == AuthStrategy::Jwt) {
        e << YAML::Key << "strategy" << YAML::Value << "jwt";
        for (const auto* field : {"algorithm", "secret", "payload"}) {
            if (auto it = actor.authConfig.find(field); it != actor.authConfig.end()) {
                e << YAML::Key << field << YAML::Value << it->second;
            }
        }
    } else if (actor.strategy == AuthStrategy::Mtls) {
        e << YAML::Key << "strategy" << YAML::Value << "mtls";
        for (const auto* field :
             {"format", "cert_path", "key_path", "key_password", "ca_cert_path"}) {
            if (auto it = actor.authConfig.find(field); it != actor.authConfig.end()) {
                e << YAML::Key << field << YAML::Value << it->second;
            }
        }
    } else {
        e << YAML::Key << "strategy" << YAML::Value << "simple";
        if (!actor.authSteps.empty()) {
            emitAuthStep(e, actor.authSteps.front(), /*isChainStep=*/false);
        }
    }
    e << YAML::EndMap;

    e << YAML::Key << "session" << YAML::Value << YAML::BeginMap << YAML::Key << "ttl"
      << YAML::Value << (std::to_string(actor.sessionTtl.count()) + "s");
    if (actor.refresh) {
        e << YAML::Key << "refresh" << YAML::Value << YAML::BeginMap << YAML::Key << "method"
          << YAML::Value << std::string{methodToString(actor.refresh->method)} << YAML::Key
          << "path" << YAML::Value << actor.refresh->pathTemplate;
        if (!actor.refresh->headers.empty()) {
            e << YAML::Key << "headers" << YAML::Value;
            emitStringMap(e, actor.refresh->headers);
        }
        if (actor.refresh->bodyTemplate) {
            e << YAML::Key << "body" << YAML::Value;
            emitBodyTemplate(e, *actor.refresh->bodyTemplate);
        }
        // List form takes precedence over scalar, as for operation-level expect_status.
        if (!actor.refresh->expectStatusList.empty()) {
            e << YAML::Key << "expect_status" << YAML::Value << YAML::Flow << YAML::BeginSeq;
            for (int const s : actor.refresh->expectStatusList) {
                e << s;
            }
            e << YAML::EndSeq;
        } else if (actor.refresh->expectStatus) {
            e << YAML::Key << "expect_status" << YAML::Value << *actor.refresh->expectStatus;
        }
        emitExtractions(e, actor.refresh->extractions);
        e << YAML::EndMap;
    }
    e << YAML::EndMap;

    if (!actor.inject.headers.empty()) {
        e << YAML::Key << "inject" << YAML::Value << YAML::BeginMap << YAML::Key << "headers"
          << YAML::Value;
        emitStringMap(e, actor.inject.headers);
        e << YAML::EndMap;
    }
    if (!actor.allowedHosts.empty()) {
        e << YAML::Key << "allowed_hosts" << YAML::Value << YAML::BeginSeq;
        for (const auto& host : actor.allowedHosts) {
            e << host;
        }
        e << YAML::EndSeq;
    }

    e << YAML::EndMap;
    return e.c_str();
}

// The largest IEC unit (KiB, MiB, GiB) that divides `bytes`, else a bare byte count; the
// parser's `max_response_size` accepts exactly these forms.
std::string byteSizeLiteral(std::size_t bytes) {
    constexpr std::array<std::pair<std::size_t, std::string_view>, 3> kUnits{{
        {std::size_t{1} << 30, "GiB"},
        {std::size_t{1} << 20, "MiB"},
        {std::size_t{1} << 10, "KiB"},
    }};
    for (const auto& [scale, suffix] : kUnits) {
        if (bytes % scale == 0) {
            return std::format("{}{}", bytes / scale, suffix);
        }
    }
    return std::to_string(bytes);
}

std::string emitEnvironment(const std::string& name,
                            const std::map<std::string, std::string>& vars,
                            const std::optional<TransportConfig>& transport) {
    YAML::Emitter e;
    e << YAML::BeginMap << YAML::Key << "name" << YAML::Value << name;
    if (!vars.empty()) {
        e << YAML::Key << "variables" << YAML::Value;
        emitStringMap(e, vars);
    }
    if (transport) {
        e << YAML::Key << "transport" << YAML::Value << YAML::BeginMap;
        // Emit only fields that differ from defaults, keeping round-tripped YAML minimal.
        const TransportConfig defaults;
        if (transport->tlsVerify != defaults.tlsVerify) {
            e << YAML::Key << "tls_verify" << YAML::Value << transport->tlsVerify;
        }
        if (transport->tlsVerifyHost != defaults.tlsVerifyHost) {
            e << YAML::Key << "tls_verify_host" << YAML::Value << transport->tlsVerifyHost;
        }
        if (transport->caBundlePath) {
            e << YAML::Key << "ca_bundle" << YAML::Value << *transport->caBundlePath;
        }
        if (transport->proxy) {
            e << YAML::Key << "proxy" << YAML::Value << *transport->proxy;
        }
        if (transport->connectTimeout != defaults.connectTimeout) {
            e << YAML::Key << "connect_timeout" << YAML::Value
              << (std::to_string(transport->connectTimeout.count()) + "ms");
        }
        if (transport->maxResponseBytes != defaults.maxResponseBytes) {
            e << YAML::Key << "max_response_size" << YAML::Value
              << byteSizeLiteral(transport->maxResponseBytes);
        }
        e << YAML::EndMap;
    }
    e << YAML::EndMap;
    return e.c_str();
}

/// The id to write: the project's own, else the one already in `rootYaml`
/// (so a save never re-keys the project's secrets), else a fresh random one.
std::string effectiveProjectId(const Project& project, const fs::path& rootYaml) {
    if (!project.id.empty()) {
        return project.id;
    }
    std::error_code ec;
    if (fs::is_regular_file(rootYaml, ec)) {
        try {
            const auto existing = YAML::LoadFile(rootYaml.string())["id"];
            if (existing && existing.IsScalar()) {
                auto id = existing.as<std::string>();
                if (validateProjectId(id)) {
                    return id;
                }
            }
        } catch (const YAML::Exception&) {
            // An unreadable existing root just means there is no id to keep.
        }
    }
    return generateProjectId();
}

std::string emitRoot(const Project& project, const std::string& projectId) {
    YAML::Emitter e;
    e << YAML::BeginMap << YAML::Key << "version" << YAML::Value << 1 << YAML::Key << "name"
      << YAML::Value << project.name << YAML::Key << "id" << YAML::Value << projectId << YAML::Key
      << "default_environment" << YAML::Value << project.defaultEnvironment;
    if (!project.allowedEnv.empty()) {
        e << YAML::Key << "allowed_env" << YAML::Value << YAML::BeginSeq;
        for (const auto& name : project.allowedEnv) {
            e << name;
        }
        e << YAML::EndSeq;
    }
    if (project.latencySloP95Ms > 0) {
        e << YAML::Key << "latency_slo" << YAML::Value << YAML::BeginMap << YAML::Key << "p95_ms"
          << YAML::Value << project.latencySloP95Ms << YAML::EndMap;
    }
    if (!project.failureMessageShow.empty()) {
        emitFailureMessage(e, project.failureMessageShow);
    }
    if (project.defaultAuth && project.defaultAuth->type != InlineAuthType::None &&
        project.defaultAuth->type != InlineAuthType::Inherit) {
        emitInlineAuth(e, *project.defaultAuth);
    }
    // A pattern for a folder with no files would warn on every reload.
    e << YAML::Key << "imports" << YAML::Value << YAML::BeginSeq;
    if (!project.actors.empty()) {
        e << "actors/*.yaml";
    }
    if (!project.resources.empty()) {
        e << "resources/*.yaml";
    }
    if (!project.environments.empty()) {
        e << "environments/*.yaml";
    }
    e << YAML::EndSeq << YAML::EndMap;
    return e.c_str();
}

}  // namespace

YamlSchemaWriter::YamlSchemaWriter() = default;
YamlSchemaWriter::~YamlSchemaWriter() = default;

SchemaWriteResult YamlSchemaWriter::write(const fs::path& targetDir,
                                          const Project& project,
                                          bool overwrite,
                                          bool pruneStale,
                                          std::map<fs::path, std::size_t>* loadedFiles) {
    // Validate every id-derived file stem first, so a bad id writes (or prunes) nothing outside
    // targetDir.
    if (!project.id.empty()) {
        if (auto ok = validateProjectId(project.id); !ok) {
            return std::unexpected(ok.error());
        }
    }
    for (const auto& id : project.actors | std::views::keys) {
        if (auto ok = checkStem("actor id", id.value); !ok) {
            return std::unexpected(ok.error());
        }
    }
    for (const auto& id : project.resources | std::views::keys) {
        if (auto ok = checkStem("resource id", id.value); !ok) {
            return std::unexpected(ok.error());
        }
    }
    for (const auto& name : project.environments | std::views::keys) {
        if (auto ok = checkStem("environment name", name); !ok) {
            return std::unexpected(ok.error());
        }
    }

    std::error_code ec;
    if (fs::exists(targetDir, ec) && !overwrite) {
        // Fail only if reqloom.yaml is already there; an existing directory alone is fine.
        if (fs::exists(targetDir / "reqloom.yaml", ec)) {
            return std::unexpected(ReqloomError{ErrorCode::SchemaInvalid,
                                                ErrorClass::Schema,
                                                "writer: reqloom.yaml exists in " +
                                                    targetDir.string() +
                                                    " (pass overwrite=true to replace)"});
        }
    }
    fs::create_directories(targetDir, ec);
    if (ec) {
        return std::unexpected(
            ReqloomError{ErrorCode::SchemaInvalid,
                         ErrorClass::Schema,
                         "writer: cannot create " + targetDir.string() + ": " + ec.message()});
    }

    // Build the full (target, content) plan first, then stage and commit in two phases so a failure
    // leaves no target touched.
    SavePlan plan;
    plan.emplace_back(targetDir / "reqloom.yaml",
                      emitRoot(project, effectiveProjectId(project, targetDir / "reqloom.yaml")));
    for (const auto& [id, actor] : project.actors) {
        plan.emplace_back(targetDir / "actors" / (id.value + ".yaml"), emitActor(actor));
    }
    for (const auto& [id, resource] : project.resources) {
        plan.emplace_back(targetDir / "resources" / (id.value + ".yaml"), emitResource(resource));
    }
    for (const auto& [name, vars] : project.environments) {
        std::optional<TransportConfig> transport;
        if (auto it = project.transport.find(name); it != project.transport.end()) {
            transport = it->second;
        }
        plan.emplace_back(targetDir / "environments" / (name + ".yaml"),
                          emitEnvironment(name, vars, transport));
    }
    for (const auto& entry : plan) {
        if (!isUnder(targetDir, entry.first)) {
            return std::unexpected(ReqloomError{ErrorCode::SchemaInvalid,
                                                ErrorClass::Schema,
                                                "writer: refusing to write " +
                                                    entry.first.string() + " outside " +
                                                    targetDir.string()});
        }
    }

    std::vector<fs::path> guardedPrune;
    if (loadedFiles != nullptr) {
        auto checked = checkAgainstLoaded(targetDir, plan, *loadedFiles, overwrite && pruneStale);
        if (!checked) {
            return std::unexpected(checked.error());
        }
        guardedPrune = std::move(*checked);
    }

    // Phase 1: stage a .tmp per changed file; on failure remove them all and abort untouched.
    struct Staged {
        fs::path target;
        fs::path temp;
    };
    std::vector<Staged> staged;
    const auto removeStaged = [&staged]() {
        for (const auto& s : staged) {
            std::error_code rmEc;
            fs::remove(s.temp, rmEc);
        }
    };
    for (const auto& [target, content] : plan) {
        if (fileHasContent(target, content)) {
            continue;
        }
        auto temp = stageTemp(target, content);
        if (!temp) {
            removeStaged();
            return std::unexpected(temp.error());
        }
        staged.push_back({target, *temp});
    }

    for (const auto& s : staged) {
        fs::rename(s.temp, s.target, ec);
        if (ec) {
            removeStaged();
            return std::unexpected(
                ReqloomError{ErrorCode::SchemaInvalid,
                             ErrorClass::Schema,
                             "writer: cannot commit " + s.target.string() + ": " + ec.message()});
        }
    }

    if (loadedFiles != nullptr) {
        finishGuardedSave(plan, guardedPrune, *loadedFiles);
        return targetDir / "reqloom.yaml";
    }

    // Prune files of entities that no longer exist so they do not reload as ghosts. Only with
    // `overwrite`, so slotting into an existing directory never deletes files the writer did not
    // create; `pruneStale=false` (`reqloom import --force`) never deletes.
    if (overwrite && pruneStale) {
        std::set<std::string> actorStems;
        for (const auto& [id, _] : project.actors) {
            actorStems.insert(id.value);
        }
        pruneStaleFiles(targetDir, targetDir / "actors", actorStems);

        std::set<std::string> resourceStems;
        for (const auto& [id, _] : project.resources) {
            resourceStems.insert(id.value);
        }
        pruneStaleFiles(targetDir, targetDir / "resources", resourceStems);

        std::set<std::string> environmentStems;
        for (const auto& [name, _] : project.environments) {
            environmentStems.insert(name);
        }
        pruneStaleFiles(targetDir, targetDir / "environments", environmentStems);
    }

    return targetDir / "reqloom.yaml";
}

}  // namespace reqloom::engine
