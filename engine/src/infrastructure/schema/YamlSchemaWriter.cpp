// YamlSchemaWriter — writes Project to reqloom.yaml.
#include "YamlSchemaWriter.h"

#include "../../domain/Codecs.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace reqloom::engine {

namespace {

// ─── HttpMethod / enum mappings ──────────────────────────────────────────────

using codecs::methodToString;

// Note: only JsonPath round-trips through the parser today (the parser
// detects header-style by `$.headers.` prefix). XPath / Regex / Cookie /
// StatusCode are emit-only stubs.

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
    }
    return "hand_written";
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

// ─── Atomic write helper ─────────────────────────────────────────────────────

// True when `target` already holds exactly `content` — lets the writer skip
// unchanged files so editing one operation doesn't churn (and bump the mtime
// of) every other project file, leaving their git status and comments alone.
[[nodiscard]] bool fileHasContent(const fs::path& target, const std::string& content) {
    std::error_code ec;
    if (!fs::exists(target, ec) || ec) {
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

// Write `content` to a `.tmp` sibling of `target` (creating parent dirs). The
// caller commits by renaming the temp onto the target. Two-phase staging makes
// a multi-file save near-atomic: if any file fails to stage, none is committed.
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
    auto temp = target;
    temp += ".tmp";
    {
        std::ofstream out{temp, std::ios::binary | std::ios::trunc};
        if (!out) {
            return std::unexpected(ReqloomError{ErrorCode::SchemaInvalid,
                                                ErrorClass::Schema,
                                                "writer: cannot open temp file " + temp.string()});
        }
        out << content;
        if (!out) {
            return std::unexpected(
                ReqloomError{ErrorCode::SchemaInvalid,
                             ErrorClass::Schema,
                             "writer: failed writing temp file " + temp.string()});
        }
    }
    return temp;
}

// ─── Emitter helpers ─────────────────────────────────────────────────────────

// Remove stale `*.yaml` files in a managed sub-directory (actors/, resources/,
// environments/) whose stem is no longer a current entity. Without this, a
// rename or delete leaves the old file on disk and it reloads as a ghost
// entity on the next parse. Only `.yaml` files are touched; unrelated files
// are left alone.
void pruneStaleFiles(const fs::path& dir, const std::set<std::string>& keepStems) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return;
    }
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) {
            break;
        }
        const auto& path = entry.path();
        if (!entry.is_regular_file() || path.extension() != ".yaml") {
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
    // std::map already iterates in ascending key order, so no copy/sort.
    for (const auto& [k, v] : m) {
        e << YAML::Key << k << YAML::Value << v;
    }
    e << YAML::EndMap;
}

// Emit a request `body`. The parser stores `body` verbatim for scalars and as
// JSON text for structured (map/sequence) bodies, and never re-encodes a
// scalar on load. So emitting the stored body as a single scalar round-trips
// exactly: yaml-cpp escapes it once for YAML, the parser unescapes it once, and
// repeated saves are idempotent (this is what fixed the over-escaping bug).
void emitBodyTemplate(YAML::Emitter& e, const std::string& bodyTemplate) {
    e << bodyTemplate;
}

// String form of an extraction source for the explicit map form below.
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

// The source the parser would auto-detect from a path prefix. When the actual
// source matches this, the compact scalar form is safe; otherwise the explicit
// map form is required so the source survives the round-trip.
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
        // Compact `var: path` when the source is re-derivable from the path;
        // otherwise the explicit `var: { path, source }` map form so XPath /
        // Regex (which have no detectable prefix) don't degrade to JsonPath.
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

void emitProvenance(YAML::Emitter& e, const Provenance& p) {
    e << YAML::Key << "_provenance" << YAML::Value << YAML::BeginMap;
    e << YAML::Key << "source" << YAML::Value << std::string{provenanceSourceToString(p.source)};
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

void emitOperation(YAML::Emitter& e, const Operation& op) {
    e << YAML::BeginMap;
    e << YAML::Key << "method" << YAML::Value << std::string{methodToString(op.method)};
    e << YAML::Key << "path" << YAML::Value << op.pathTemplate;
    if (!op.actor.value.empty()) {
        e << YAML::Key << "actor" << YAML::Value << op.actor.value;
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
    // Emit `retry` only when it diverges from the defaults (max 3, backoff
    // 500ms), so a default-retry op doesn't acquire a stray block. The parser
    // reads `max` + `backoff`; emit both so the policy round-trips.
    if (op.retry.maxAttempts != RetryPolicy{}.maxAttempts ||
        op.retry.baseBackoff != RetryPolicy{}.baseBackoff) {
        e << YAML::Key << "retry" << YAML::Value << YAML::BeginMap << YAML::Key << "max"
          << YAML::Value << op.retry.maxAttempts << YAML::Key << "backoff" << YAML::Value
          << static_cast<int>(op.retry.baseBackoff.count()) << YAML::EndMap;
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
        // Values may contain {{X.y}} references resolved at run time.
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
        // List form takes precedence over scalar — same convention as
        // operation-level expect_status.
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

    e << YAML::EndMap;
    return e.c_str();
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
        // Emit only fields that diverge from defaults — keeps round-tripped
        // YAMLs minimal and lets readers see what's actually overridden.
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
        e << YAML::EndMap;
    }
    e << YAML::EndMap;
    return e.c_str();
}

std::string emitRoot(const Project& project) {
    YAML::Emitter e;
    e << YAML::BeginMap << YAML::Key << "version" << YAML::Value << 1 << YAML::Key << "name"
      << YAML::Value << project.name << YAML::Key << "default_environment" << YAML::Value
      << project.defaultEnvironment << YAML::Key << "imports" << YAML::Value << YAML::BeginSeq
      << "actors/*.yaml"
      << "resources/*.yaml"
      << "environments/*.yaml" << YAML::EndSeq << YAML::EndMap;
    return e.c_str();
}

}  // namespace

YamlSchemaWriter::YamlSchemaWriter() = default;
YamlSchemaWriter::~YamlSchemaWriter() = default;

SchemaWriteResult YamlSchemaWriter::write(const fs::path& targetDir,
                                          const Project& project,
                                          bool overwrite) {
    std::error_code ec;
    if (fs::exists(targetDir) && !overwrite) {
        // Only fail if reqloom.yaml is already there — the directory
        // existing alone is fine (lets the writer slot into an existing project).
        if (fs::exists(targetDir / "reqloom.yaml")) {
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

    // Build the full (target, content) plan up front, then stage + commit in
    // two phases so a multi-file save is near-atomic: if any file fails to
    // stage, nothing is committed and no target file is touched.
    std::vector<std::pair<fs::path, std::string>> plan;
    plan.emplace_back(targetDir / "reqloom.yaml", emitRoot(project));
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

    // Phase 1 — stage: write a .tmp for each changed file. Skip unchanged
    // files. On any failure, remove every staged temp and abort untouched.
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

    // Phase 2 — commit: rename each staged temp onto its target.
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

    // Prune files for entities that no longer exist (rename/delete), so a
    // stale actor/resource/environment .yaml doesn't reload as a ghost. Gated
    // on overwrite — the in-place "save the project" path — so the slot-into-
    // an-existing-directory case (overwrite=false) never deletes pre-existing
    // files the writer didn't create this run.
    if (overwrite) {
        std::set<std::string> actorStems;
        for (const auto& [id, _] : project.actors) {
            actorStems.insert(id.value);
        }
        pruneStaleFiles(targetDir / "actors", actorStems);

        std::set<std::string> resourceStems;
        for (const auto& [id, _] : project.resources) {
            resourceStems.insert(id.value);
        }
        pruneStaleFiles(targetDir / "resources", resourceStems);

        std::set<std::string> environmentStems;
        for (const auto& [name, _] : project.environments) {
            environmentStems.insert(name);
        }
        pruneStaleFiles(targetDir / "environments", environmentStems);
    }

    return targetDir / "reqloom.yaml";
}

}  // namespace reqloom::engine
