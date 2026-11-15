// What a pre_request / post_response hook may see, and how its header edits are applied.

#include "HookScope.h"

// ExecutionEngine.h (for Project) holds the engine dependencies by unique_ptr.
#include "../infrastructure/hooks/HookRunner.h"
#include "../infrastructure/http/HttpClient.h"
#include "../infrastructure/schema/SchemaParser.h"
#include "../infrastructure/secrets/SecretStore.h"
#include "../infrastructure/storage/HistoryStore.h"

#include <reqloom/engine/Actor.h>

#include <cctype>
#include <regex>
#include <string>
#include <string_view>

namespace reqloom::engine {

namespace {

[[nodiscard]] bool isNameChar(const char c) noexcept {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-';
}

/// Every `scope.NAME` inside a `{{…}}` placeholder of `text`, builtin arguments included.
void collectScoped(const std::string_view text,
                   const std::string_view scope,
                   std::set<std::string>& out) {
    for (auto open = text.find("{{"); open != std::string_view::npos;
         open = text.find("{{", open + 2)) {
        const auto close = text.find("}}", open + 2);
        if (close == std::string_view::npos) {
            return;
        }
        const auto inner = text.substr(open + 2, close - open - 2);
        for (auto at = inner.find(scope); at != std::string_view::npos;
             at = inner.find(scope, at + 1)) {
            const auto dot = at + scope.size();
            const bool boundary = at == 0 || (!isNameChar(inner[at - 1]) && inner[at - 1] != '.');
            if (!boundary || dot >= inner.size() || inner[dot] != '.') {
                continue;
            }
            auto end = dot + 1;
            while (end < inner.size() && isNameChar(inner[end])) {
                ++end;
            }
            if (end > dot + 1) {
                out.emplace(inner.substr(dot + 1, end - dot - 1));
            }
        }
    }
}

void appendInlineAuth(const InlineAuth& a, std::vector<std::string_view>& out) {
    for (const auto* field : {&a.token,           &a.username,           &a.password,
                              &a.apiKeyName,      &a.apiKeyValue,        &a.awsAccessKey,
                              &a.awsSecretKey,    &a.awsRegion,          &a.awsService,
                              &a.awsSessionToken, &a.oauthConsumerKey,   &a.oauthConsumerSecret,
                              &a.oauthToken,      &a.oauthTokenSecret,   &a.oauth2TokenUrl,
                              &a.oauth2ClientId,  &a.oauth2ClientSecret, &a.oauth2Scope,
                              &a.oauth2AuthUrl,   &a.oauth2CallbackUrl,  &a.oauth2AccessToken,
                              &a.jwtSecret,       &a.jwtPayload,         &a.mtlsCertPath,
                              &a.mtlsKeyPath,     &a.mtlsKeyPassword,    &a.mtlsCaCertPath}) {
        out.emplace_back(*field);
    }
}

void appendActorTexts(const Actor& actor, std::vector<std::string_view>& texts) {
    for (const auto& [_, v] : actor.authConfig) {
        texts.emplace_back(v);
    }
    for (const auto& [_, v] : actor.inject.headers) {
        texts.emplace_back(v);
    }
    const auto appendRequest = [&texts](const auto& request) {
        texts.emplace_back(request.pathTemplate);
        if (request.bodyTemplate) {
            texts.emplace_back(*request.bodyTemplate);
        }
        for (const auto& [_, v] : request.headers) {
            texts.emplace_back(v);
        }
    };
    for (const auto& step : actor.authSteps) {
        appendRequest(step);
    }
    if (actor.refresh) {
        appendRequest(*actor.refresh);
    }
}

[[nodiscard]] std::vector<std::string_view> operationTexts(const Operation& op,
                                                           const Project& project) {
    std::vector<std::string_view> texts{op.pathTemplate};
    for (const auto& [_, v] : op.headers) {
        texts.emplace_back(v);
    }
    for (const auto& [_, v] : op.queryParams) {
        texts.emplace_back(v);
    }
    if (op.bodyTemplate) {
        texts.emplace_back(*op.bodyTemplate);
    }
    if (op.bodyForm) {
        for (const auto& [_, v] : *op.bodyForm) {
            texts.emplace_back(v);
        }
    }
    if (op.pollUntil) {
        texts.emplace_back(op.pollUntil->pathTemplate);
    }
    if (op.inlineAuth) {
        if (op.inlineAuth->type != InlineAuthType::Inherit) {
            appendInlineAuth(*op.inlineAuth, texts);
        } else if (project.defaultAuth) {
            appendInlineAuth(*project.defaultAuth, texts);
        }
    }
    if (const auto actorIt = project.actors.find(op.actor); actorIt != project.actors.end()) {
        appendActorTexts(actorIt->second, texts);
    }
    return texts;
}

void collectScriptSecrets(const std::string_view script, std::set<std::string>& out) {
    static const std::regex pattern(
        R"(\bsecret\s*(?:\.\s*([A-Za-z_$][\w$]*)|\[\s*(["'])([^"'\\]+)\2\s*\]))");
    const std::string text{script};
    for (std::sregex_iterator it{text.begin(), text.end(), pattern}, end; it != end; ++it) {
        out.insert((*it)[1].matched ? (*it)[1].str() : (*it)[3].str());
    }
}

}  // namespace

std::set<std::string> hookSecretNames(const Operation& op,
                                      const Project& project,
                                      const std::map<std::string, std::string>& envVars,
                                      const std::string_view script) {
    std::set<std::string> secrets;
    std::set<std::string> envNames;
    for (const auto text : operationTexts(op, project)) {
        collectScoped(text, "secret", secrets);
        collectScoped(text, "env", envNames);
    }
    // A `key: !secret NAME` environment entry holds `{{secret.NAME}}`.
    for (const auto& name : envNames) {
        if (const auto it = envVars.find(name); it != envVars.end()) {
            collectScoped(it->second, "secret", secrets);
        }
    }
    collectScriptSecrets(script, secrets);
    return secrets;
}

std::set<std::string> hookScriptSecretNames(const std::string_view script) {
    std::set<std::string> secrets;
    collectScriptSecrets(script, secrets);
    return secrets;
}

std::vector<std::pair<std::string, std::string>> mergeHookHeaders(
    const std::vector<std::pair<std::string, std::string>>& original,
    const std::map<std::string, std::string>& exposed,
    const std::map<std::string, std::string>& mutated) {
    if (mutated == exposed) {
        return original;
    }
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(original.size() + mutated.size());
    std::set<std::string> replaced;
    for (const auto& [name, value] : original) {
        const auto now = mutated.find(name);
        if (now == mutated.end()) {
            continue;
        }
        const auto before = exposed.find(name);
        if (before != exposed.end() && before->second == now->second) {
            out.emplace_back(name, value);
        } else if (replaced.insert(name).second) {
            out.emplace_back(name, now->second);
        }
    }
    for (const auto& [name, value] : mutated) {
        if (!exposed.contains(name)) {
            out.emplace_back(name, value);
        }
    }
    return out;
}

}  // namespace reqloom::engine
