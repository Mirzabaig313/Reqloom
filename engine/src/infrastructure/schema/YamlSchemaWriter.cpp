// YamlSchemaWriter — writes Project to chainapi.yaml.
#include "YamlSchemaWriter.h"

#include "../../domain/Codecs.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace chainapi::engine {

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

std::expected<void, ChainApiError> writeAtomic(const fs::path& target, const std::string& content) {
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    if (ec) {
        return std::unexpected(ChainApiError{
            ErrorCode::SchemaInvalid,
            ErrorClass::Schema,
            "writer: cannot create dir " + target.parent_path().string() + ": " + ec.message()});
    }

    auto temp = target;
    temp += ".tmp";
    {
        std::ofstream out{temp, std::ios::binary | std::ios::trunc};
        if (!out) {
            return std::unexpected(ChainApiError{ErrorCode::SchemaInvalid,
                                                 ErrorClass::Schema,
                                                 "writer: cannot open temp file " + temp.string()});
        }
        out << content;
        if (!out) {
            return std::unexpected(
                ChainApiError{ErrorCode::SchemaInvalid,
                              ErrorClass::Schema,
                              "writer: failed writing temp file " + temp.string()});
        }
    }

    fs::rename(temp, target, ec);
    if (ec) {
        std::error_code _;
        fs::remove(temp, _);
        return std::unexpected(ChainApiError{ErrorCode::SchemaInvalid,
                                             ErrorClass::Schema,
                                             "writer: cannot rename " + temp.string() + " → " +
                                                 target.string() + ": " + ec.message()});
    }
    return {};
}

// ─── Emitter helpers ─────────────────────────────────────────────────────────

void emitStringMap(YAML::Emitter& e, const std::map<std::string, std::string>& m) {
    e << YAML::BeginMap;
    std::vector<std::pair<std::string, std::string>> sorted{m.begin(), m.end()};
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });
    for (const auto& [k, v] : sorted) {
        e << YAML::Key << k << YAML::Value << v;
    }
    e << YAML::EndMap;
}

void emitExtractions(YAML::Emitter& e, const std::vector<Extraction>& extractions) {
    if (extractions.empty()) return;
    e << YAML::Key << "extract" << YAML::Value << YAML::BeginMap;
    for (const auto& ext : extractions) {
        e << YAML::Key << ext.variableName << YAML::Value << ext.sourcePath;
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
    if (p.model) e << YAML::Key << "model" << YAML::Value << *p.model;
    if (p.importedAt) e << YAML::Key << "imported_at" << YAML::Value << *p.importedAt;
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
        e << YAML::Key << "body" << YAML::Value << *op.bodyTemplate;
    }
    if (op.bodyForm) {
        e << YAML::Key << "body_form" << YAML::Value;
        emitStringMap(e, *op.bodyForm);
    }
    if (!op.expectStatusList.empty()) {
        e << YAML::Key << "expect_status" << YAML::Value << YAML::Flow << YAML::BeginSeq;
        for (int s : op.expectStatusList) e << s;
        e << YAML::EndSeq;
    } else if (op.expectStatus) {
        e << YAML::Key << "expect_status" << YAML::Value << *op.expectStatus;
    }
    if (!op.explicitDependencies.empty()) {
        e << YAML::Key << "depends_on" << YAML::Value << YAML::Flow << YAML::BeginSeq;
        for (const auto& dep : op.explicitDependencies) e << dep.value;
        e << YAML::EndSeq;
    }
    if (op.preRequestScript) {
        e << YAML::Key << "pre_request" << YAML::Value << YAML::Literal << *op.preRequestScript;
    }
    if (op.postResponseScript) {
        e << YAML::Key << "post_response" << YAML::Value << YAML::Literal << *op.postResponseScript;
    }
    if (op.timeout) {
        e << YAML::Key << "timeout" << YAML::Value << static_cast<int>(op.timeout->count());
    }
    if (op.force) {
        e << YAML::Key << "force" << YAML::Value << true;
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
    for (const auto& [k, _] : res.operations) opNames.push_back(k);
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
        e << YAML::Key << "body" << YAML::Value << *step.bodyTemplate;
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
            e << YAML::Key << "body" << YAML::Value << *actor.refresh->bodyTemplate;
        }
        // List form takes precedence over scalar — same convention as
        // operation-level expect_status.
        if (!actor.refresh->expectStatusList.empty()) {
            e << YAML::Key << "expect_status" << YAML::Value << YAML::Flow << YAML::BeginSeq;
            for (int s : actor.refresh->expectStatusList) e << s;
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
                            const std::map<std::string, std::string>& vars) {
    YAML::Emitter e;
    e << YAML::BeginMap << YAML::Key << "name" << YAML::Value << name;
    if (!vars.empty()) {
        e << YAML::Key << "variables" << YAML::Value;
        emitStringMap(e, vars);
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
        // Only fail if chainapi.yaml is already there — the directory
        // existing alone is fine (lets the writer slot into an existing project).
        if (fs::exists(targetDir / "chainapi.yaml")) {
            return std::unexpected(ChainApiError{ErrorCode::SchemaInvalid,
                                                 ErrorClass::Schema,
                                                 "writer: chainapi.yaml exists in " +
                                                     targetDir.string() +
                                                     " (pass overwrite=true to replace)"});
        }
    }
    fs::create_directories(targetDir, ec);
    if (ec) {
        return std::unexpected(
            ChainApiError{ErrorCode::SchemaInvalid,
                          ErrorClass::Schema,
                          "writer: cannot create " + targetDir.string() + ": " + ec.message()});
    }

    if (auto r = writeAtomic(targetDir / "chainapi.yaml", emitRoot(project)); !r) {
        return std::unexpected(r.error());
    }

    for (const auto& [id, actor] : project.actors) {
        const auto path = targetDir / "actors" / (id.value + ".yaml");
        if (auto r = writeAtomic(path, emitActor(actor)); !r) {
            return std::unexpected(r.error());
        }
    }

    for (const auto& [id, resource] : project.resources) {
        const auto path = targetDir / "resources" / (id.value + ".yaml");
        if (auto r = writeAtomic(path, emitResource(resource)); !r) {
            return std::unexpected(r.error());
        }
    }

    for (const auto& [name, vars] : project.environments) {
        const auto path = targetDir / "environments" / (name + ".yaml");
        if (auto r = writeAtomic(path, emitEnvironment(name, vars)); !r) {
            return std::unexpected(r.error());
        }
    }

    return targetDir / "chainapi.yaml";
}

}  // namespace chainapi::engine
