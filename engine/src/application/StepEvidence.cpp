// StepEvidence — see header.

#include "StepEvidence.h"

#include "../domain/Codecs.h"
#include "../domain/SecretRegistry.h"
#include "../infrastructure/http/HttpClient.h"
#include "HeaderMasking.h"
#include "PredicateEvaluator.h"
#include "SecretRedaction.h"

#include <reqloom/engine/ExecutionEngine.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace reqloom::engine {

namespace {

[[nodiscard]] std::string_view trimmed(std::string_view s) noexcept {
    const auto begin = s.find_first_not_of(" \t");
    if (begin == std::string_view::npos) {
        return {};
    }
    return s.substr(begin, s.find_last_not_of(" \t") - begin + 1);
}

[[nodiscard]] bool equalsIgnoreCase(std::string_view a, std::string_view b) noexcept {
    return std::ranges::equal(a, b, [](const char x, const char y) {
        return std::tolower(static_cast<unsigned char>(x)) ==
               std::tolower(static_cast<unsigned char>(y));
    });
}

/// Every `<scope>.<field>` reference in `text` other than `secret.`, `env.` and `$` built-ins
/// (whose argument counts), with an index (`order[0]`) dropped from the scope.
void collectResourceReferences(std::string_view text, std::set<std::string>& out) {
    std::size_t pos{};
    while ((pos = text.find("{{", pos)) != std::string_view::npos) {
        const auto close = text.find("}}", pos + 2);
        if (close == std::string_view::npos) {
            return;
        }
        auto ref = trimmed(text.substr(pos + 2, close - pos - 2));
        pos = close + 2;
        if (ref.starts_with('$')) {
            const auto open = ref.find('(');
            if (open == std::string_view::npos || !ref.ends_with(')')) {
                continue;
            }
            ref = trimmed(ref.substr(open + 1, ref.size() - open - 2));
        }
        const auto dot = ref.find('.');
        if (dot == std::string_view::npos || dot == 0 || dot + 1 == ref.size()) {
            continue;
        }
        auto scope = ref.substr(0, dot);
        scope = scope.substr(0, scope.find('['));
        if (scope == "secret" || scope == "env" || scope.starts_with('$')) {
            continue;
        }
        out.insert(std::string{scope} + "." + std::string{ref.substr(dot + 1)});
    }
}

using NamePredicate = bool (*)(std::string_view) noexcept;

/// References in the values of `fields` whose name `isCredential` accepts.
void collectNamed(const std::map<std::string, std::string>& fields,
                  const NamePredicate isCredential,
                  std::set<std::string>& out) {
    for (const auto& [name, value] : fields) {
        if (isCredential(name)) {
            collectResourceReferences(value, out);
        }
    }
}

void collectJsonBody(const nlohmann::json& node, std::set<std::string>& out) {
    if (node.is_object()) {
        for (const auto& [key, value] : node.items()) {
            if (isCredentialBodyField(key) && value.is_string()) {
                collectResourceReferences(value.get_ref<const std::string&>(), out);
            } else {
                collectJsonBody(value, out);
            }
        }
    } else if (node.is_array()) {
        for (const auto& element : node) {
            collectJsonBody(element, out);
        }
    }
}

/// A JSON body template's credential-named fields; a template that is not JSON adds nothing.
void collectBodyTemplate(const std::optional<std::string>& body, std::set<std::string>& out) {
    if (!body || jsonNestingExceeds(*body)) {
        return;
    }
    const auto doc = nlohmann::json::parse(*body, nullptr, /*allow_exceptions=*/false);
    if (!doc.is_discarded()) {
        collectJsonBody(doc, out);
    }
}

void collectInlineAuth(const InlineAuth& auth, std::set<std::string>& out) {
    for (const std::string* field : {&auth.token,
                                     &auth.password,
                                     &auth.apiKeyValue,
                                     &auth.awsAccessKey,
                                     &auth.awsSecretKey,
                                     &auth.awsSessionToken,
                                     &auth.oauthConsumerSecret,
                                     &auth.oauthToken,
                                     &auth.oauthTokenSecret,
                                     &auth.oauth2ClientSecret,
                                     &auth.oauth2AccessToken,
                                     &auth.jwtSecret,
                                     &auth.mtlsKeyPassword}) {
        collectResourceReferences(*field, out);
    }
}

void collectActor(const Actor& actor, std::set<std::string>& out) {
    for (const auto& [name, value] : actor.authConfig) {
        collectResourceReferences(value, out);
    }
    collectNamed(actor.inject.headers, isCredentialHeader, out);
    for (const auto& step : actor.authSteps) {
        collectNamed(step.headers, isCredentialHeader, out);
        collectBodyTemplate(step.bodyTemplate, out);
    }
    if (actor.refresh) {
        collectNamed(actor.refresh->headers, isCredentialHeader, out);
        collectBodyTemplate(actor.refresh->bodyTemplate, out);
    }
}

/// `name=value` pairs separated by `separator`; each value whose name is sensitive is registered.
void registerSensitivePairs(std::string_view text,
                            const char separator,
                            const NamePredicate isCredential,
                            SecretRegistry& registry) {
    while (!text.empty()) {
        const auto end = text.find(separator);
        const auto pair = trimmed(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        const auto eq = pair.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }
        const auto rawName = pair.substr(0, eq);
        const auto rawValue = pair.substr(eq + 1);
        const auto name = codecs::urlDecode(rawName).value_or(std::string{rawName});
        if (isCredential == nullptr || isCredential(name)) {
            registry.add(rawValue);
            registry.add(codecs::urlDecode(rawValue).value_or(std::string{rawValue}));
        }
    }
}

void registerHeader(std::string_view name, std::string_view value, SecretRegistry& registry) {
    if (equalsIgnoreCase(name, "cookie")) {
        // Every cookie is treated as a session credential: no name reliably tells a session
        // cookie (PHPSESSID, connect.sid, _app_session, .AspNetCore.Cookies, …) from a preference.
        registerSensitivePairs(value, ';', nullptr, registry);
        return;
    }
    registry.add(value);
    const auto space = value.find(' ');
    if (space == std::string_view::npos) {
        return;
    }
    // `Bearer <token>`, `Basic <base64>`, `Token <key>`: the credential follows the scheme word.
    const auto credential = trimmed(value.substr(space + 1));
    registry.add(credential);
    if (equalsIgnoreCase(value.substr(0, space), "basic")) {
        if (const auto decoded = codecs::base64Decode(credential)) {
            registry.add(*decoded);
            if (const auto colon = decoded->find(':'); colon != std::string::npos) {
                registry.add(std::string_view{*decoded}.substr(colon + 1));
            }
        }
    }
}

void registerJsonFields(const nlohmann::json& node, SecretRegistry& registry) {
    if (node.is_object()) {
        for (const auto& [key, value] : node.items()) {
            if (isCredentialBodyField(key) && value.is_string()) {
                registry.add(value.get_ref<const std::string&>());
            } else {
                registerJsonFields(value, registry);
            }
        }
    } else if (node.is_array()) {
        for (const auto& element : node) {
            registerJsonFields(element, registry);
        }
    }
}

void registerBody(std::string_view body, SecretRegistry& registry) {
    const auto first = body.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return;
    }
    if (body[first] == '{' || body[first] == '[') {
        // The walk recurses per level: a hostile body nested past the limit is not walked.
        if (jsonNestingExceeds(body)) {
            return;
        }
        const auto doc = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
        if (!doc.is_discarded()) {
            registerJsonFields(doc, registry);
        }
        return;
    }
    if (body.find_first_of(" \r\n") == std::string_view::npos) {
        registerSensitivePairs(body, '&', isCredentialBodyField, registry);
    }
}

/// `text` cut to `limit` bytes on a UTF-8 boundary; `cut` says whether anything was dropped.
[[nodiscard]] std::string cutTo(std::string text, const std::size_t limit, bool& cut) {
    cut = text.size() > limit;
    return cut ? codecs::truncateUtf8(text, limit) : std::move(text);
}

void finalizeResponse(StepResponse& response, const RunContext& ctx, const std::size_t limit) {
    response.limitBytes = limit;
    const bool keptAll = response.body.size() == response.totalBytes;
    if (!keptAll && response.body.empty()) {
        // Nothing kept (a limit of 0, or the run's retention budget spent).
        response.truncated = true;
        return;
    }
    std::optional<std::string> masked{};
    try {
        masked = maskCapturedBody(response.body, ctx);
    } catch (...) {
        // Fail closed: a body that could not be masked is withheld.
        masked = std::nullopt;
    }
    if (!masked) {
        response.binary = true;
        response.truncated = false;
        response.body.clear();
        return;
    }
    if (!keptAll) {
        // The kept part may end inside a secret that therefore matched nothing: drop as much as
        // the longest secret from the end, so no prefix of one survives.
        const auto longest = ctx.secretRegistry().longestPattern();
        masked->resize(masked->size() > longest ? masked->size() - longest : 0);
        *masked = codecs::truncateUtf8(*masked, masked->size());
    }
    response.body = cutTo(std::move(*masked), limit, response.truncated);
    response.truncated = response.truncated || !keptAll;
}

void finalizeExtracted(ExtractedValue& extracted, const SecretRegistry& registry) {
    std::string masked{kRedactedValueMarker};
    if (!isAgentSensitiveName(extracted.name)) {
        // An object or list extraction is JSON text: mask its credential fields by name, and
        // secrets in its string values (decoded, so an escaped `"` or `\` still matches).
        std::optional<std::string> fields{};
        try {
            fields = redactSensitiveBodyFields(extracted.value, MaskProfile::Agent, &registry);
        } catch (...) {
            fields = std::nullopt;
        }
        if (fields) {
            masked = std::move(*fields);
        }
    }
    extracted.value = cutTo(std::move(masked), kExtractedValueBytes, extracted.truncated);
}

}  // namespace

std::set<std::string> credentialReferences(const Project& project) {
    std::set<std::string> out{};
    for (const auto& resource : project.resources | std::views::values) {
        for (const auto& op : resource.operations | std::views::values) {
            collectNamed(op.headers, isCredentialHeader, out);
            collectNamed(op.queryParams, isCredentialName, out);
            if (op.bodyForm) {
                collectNamed(*op.bodyForm, isCredentialBodyField, out);
            }
            collectBodyTemplate(op.bodyTemplate, out);
            if (op.inlineAuth) {
                collectInlineAuth(*op.inlineAuth, out);
            }
        }
    }
    if (project.defaultAuth) {
        collectInlineAuth(*project.defaultAuth, out);
    }
    for (const auto& actor : project.actors | std::views::values) {
        collectActor(actor, out);
    }
    return out;
}

void registerSentCredentials(const HttpRequest& req, SecretRegistry& registry) {
    for (const auto& [name, value] : req.headers) {
        if (isCredentialHeader(name)) {
            registerHeader(name, value, registry);
        }
    }
    const std::string_view url{req.url};
    if (const auto query = url.find('?'); query != std::string_view::npos) {
        const auto fragment = url.find('#', query);
        registerSensitivePairs(
            url.substr(
                query + 1,
                fragment == std::string_view::npos ? std::string_view::npos : fragment - query - 1),
            '&',
            isCredentialName,
            registry);
    }
    if (req.body) {
        registerBody(*req.body, registry);
    }
}

void keepResponseBody(StepResult& row, const std::string& body, const std::size_t limit) {
    StepResponse response{};
    response.totalBytes = body.size();
    if (limit > 0) {
        // Enough past the limit for masking to see the fields around the cut; never the lot.
        const auto keep = std::min(limit + kRetentionMarginBytes, kMaxCapturedBodyBytes);
        response.body = body.size() > keep ? codecs::truncateUtf8(body, keep) : body;
    }
    row.response = std::move(response);
}

void registerSetCookies(const std::vector<std::pair<std::string, std::string>>& headers,
                        SecretRegistry& registry) {
    for (const auto& [name, value] : headers) {
        if (!equalsIgnoreCase(name, "set-cookie")) {
            continue;
        }
        const std::string_view text{value};
        const auto pair = text.substr(0, text.find(';'));
        if (const auto eq = pair.find('='); eq != std::string_view::npos) {
            registry.add(trimmed(pair.substr(eq + 1)));
        }
    }
}

void finalizeStepEvidence(std::vector<StepResult>& steps,
                          const RunContext& ctx,
                          const std::size_t bodyLimit) {
    const auto& registry = ctx.secretRegistry();
    const auto limit = std::min(bodyLimit, kMaxFailureExcerptBytes);
    for (auto& step : steps) {
        redactStepResult(step, registry);
        if (step.response) {
            finalizeResponse(*step.response, ctx, limit);
        }
        for (auto& extracted : step.extracted) {
            finalizeExtracted(extracted, registry);
        }
    }
}

StepResult withoutEvidence(StepResult step) {
    step.response.reset();
    step.extracted.clear();
    return step;
}

}  // namespace reqloom::engine
