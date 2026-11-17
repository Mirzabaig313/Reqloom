// HostGate — see header.
#include "HostGate.h"

#include "../domain/Codecs.h"
#include "../domain/SecretRegistry.h"
#include "HeaderMasking.h"
#include "PredicateEvaluator.h"

#include <reqloom/engine/ExecutionEngine.h>
#include <reqloom/engine/HostApproval.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <ranges>
#include <string_view>
#include <utility>

namespace reqloom::engine {

namespace {

[[nodiscard]] ReqloomError refusal(std::string detail) {
    return ReqloomError{ErrorCode::HostNotApproved, ErrorClass::Auth, std::move(detail)};
}

[[nodiscard]] bool equalsIgnoreCase(std::string_view a, std::string_view b) noexcept {
    return std::ranges::equal(a, b, [](const char x, const char y) {
        return std::tolower(static_cast<unsigned char>(x)) ==
               std::tolower(static_cast<unsigned char>(y));
    });
}

[[nodiscard]] std::string_view trimmed(std::string_view s) noexcept {
    const auto begin = s.find_first_not_of(" \t");
    if (begin == std::string_view::npos) {
        return {};
    }
    return s.substr(begin, s.find_last_not_of(" \t") - begin + 1);
}

/// Whether `name=value&…` text has a non-empty value under a credential name.
[[nodiscard]] bool pairsCarryCredential(
    std::string_view text,
    const char separator,
    bool (*isCredential)(std::string_view) noexcept = isCredentialBodyField) {
    while (!text.empty()) {
        const auto end = text.find(separator);
        const auto pair = trimmed(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        const auto eq = pair.find('=');
        if (eq == std::string_view::npos || eq + 1 == pair.size()) {
            continue;
        }
        const auto rawName = pair.substr(0, eq);
        const auto name = codecs::urlDecode(rawName).value_or(std::string{rawName});
        if (isCredential(name)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool jsonCarriesCredential(const nlohmann::json& node) {
    if (node.is_object()) {
        for (const auto& [key, value] : node.items()) {
            if (isCredentialBodyField(key) && value.is_string() &&
                !value.get_ref<const std::string&>().empty()) {
                return true;
            }
            if (jsonCarriesCredential(value)) {
                return true;
            }
        }
    } else if (node.is_array()) {
        return std::ranges::any_of(node,
                                   [](const auto& item) { return jsonCarriesCredential(item); });
    }
    return false;
}

[[nodiscard]] bool bodyCarriesCredential(std::string_view body) {
    const auto first = body.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return false;
    }
    if (body[first] == '{' || body[first] == '[') {
        if (jsonNestingExceeds(body)) {
            return true;  // cannot be walked safely: assume the worst
        }
        const auto doc = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
        return !doc.is_discarded() && jsonCarriesCredential(doc);
    }
    return body.find_first_of(" \r\n") == std::string_view::npos && pairsCarryCredential(body, '&');
}

[[nodiscard]] std::string_view queryOf(std::string_view url) noexcept {
    const auto query = url.find('?');
    if (query == std::string_view::npos) {
        return {};
    }
    const auto fragment = url.find('#', query);
    return url.substr(
        query + 1,
        fragment == std::string_view::npos ? std::string_view::npos : fragment - query - 1);
}

[[nodiscard]] std::string actorLabel(const Actor& actor) {
    return "actor '" + actor.id.value + "'";
}

[[nodiscard]] std::string joined(const std::vector<std::string>& items) {
    std::string out;
    for (const auto& item : items) {
        out += (out.empty() ? "" : ", ") + item;
    }
    return out;
}

/// Header names whose value a hop may carry without inheriting the credential status, when the
/// value also looks like what those headers normally hold (`safeHeaderValue`): content
/// negotiation and connection handling, set by Reqloom or libcurl or by projects to ordinary
/// media types. Anything else (a custom header, Authorization) could hold a transformed secret.
constexpr std::array<std::string_view, 8> kSafeHopHeaders{"accept",
                                                          "accept-encoding",
                                                          "accept-language",
                                                          "cache-control",
                                                          "connection",
                                                          "content-type",
                                                          "pragma",
                                                          "user-agent"};

/// A short value of media-type or token characters with no long run of letters and digits, so a
/// hex or base64 encoded secret placed in an allowlisted header still counts as supplied data.
[[nodiscard]] bool safeHeaderValue(std::string_view value) noexcept {
    if (value.size() > 128) {
        return false;
    }
    std::size_t run = 0;
    for (const char c : value) {
        const auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u) != 0) {
            if (++run >= 16) {
                return false;
            }
            continue;
        }
        run = 0;
        if (std::string_view{" ,;=./+*-_()"}.find(c) == std::string_view::npos) {
            return false;
        }
    }
    return true;
}

/// Whether a redirect hop still carries something the original request supplied: a header outside
/// the safe list (or with an unusual value), any body or multipart part, or a value of the
/// original query string echoed in the hop URL.
[[nodiscard]] bool carriesSuppliedData(const HttpRequest& hop, std::string_view originalQuery) {
    for (const auto& [name, value] : hop.headers) {
        std::string lower{name};
        std::ranges::transform(lower, lower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        const bool safe = std::ranges::find(kSafeHopHeaders, lower) != kSafeHopHeaders.end() &&
                          safeHeaderValue(value);
        if (!value.empty() && !safe) {
            return true;
        }
    }
    if ((hop.body && !hop.body->empty()) || !hop.multipart.empty()) {
        return true;
    }
    while (!originalQuery.empty()) {
        const auto end = originalQuery.find('&');
        const auto pair = originalQuery.substr(0, end);
        originalQuery =
            end == std::string_view::npos ? std::string_view{} : originalQuery.substr(end + 1);
        const auto eq = pair.find('=');
        const auto value = eq == std::string_view::npos ? pair : pair.substr(eq + 1);
        if (!value.empty() && hop.url.find(value) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

HostGate::HostGate(Config config) : config_(std::move(config)) {
    if (auto key = hostKeyForUrl(config_.baseUrl)) {
        baseHost_ = std::move(*key);
    }
}

std::optional<std::string> HostGate::credentialIn(const HttpRequest& request,
                                                  const SendPurpose& purpose) const {
    // A local file can hold anything (an uploaded `~/.aws/credentials`): every file part counts,
    // and the person is told which file.
    std::string file;
    for (const auto& part : request.multipart) {
        if (part.isFile()) {
            file = "the local file '" + *part.filePath + "'";
            break;
        }
    }
    const auto withFile = [&file](std::string what) {
        return file.empty() ? what : what + " and " + file;
    };
    if (purpose.carriesCredential) {
        if (!purpose.reason.empty()) {
            return withFile(purpose.reason);
        }
        return withFile(!purpose.actors.empty()
                            ? "the credentials of " + actorLabel(*purpose.actors.front())
                            : std::string{"a credential (token request)"});
    }
    if (!file.empty()) {
        return file;
    }
    const auto hasSecret = [this](std::string_view text) {
        if (config_.registry != nullptr && config_.registry->containsAny(text)) {
            return true;
        }
        return std::ranges::any_of(config_.shortSecrets, [text](const std::string& secret) {
            return !secret.empty() && text.find(secret) != std::string_view::npos;
        });
    };
    for (const auto& [name, value] : request.headers) {
        if (value.empty()) {
            continue;
        }
        if (equalsIgnoreCase(name, "cookie")) {
            return "a session cookie";
        }
        if (isCredentialHeader(name)) {
            return "a credential in the " + name + " header";
        }
        if (hasSecret(value)) {
            return "a secret in the " + name + " header";
        }
    }
    if (pairsCarryCredential(queryOf(request.url), '&', isCredentialName)) {
        return "a credential in the query string";
    }
    if (hasSecret(request.url)) {
        return "a secret in the URL";
    }
    if (request.body && (bodyCarriesCredential(*request.body) || hasSecret(*request.body))) {
        return "a credential in the request body";
    }
    for (const auto& part : request.multipart) {
        if ((!part.isFile() && isCredentialBodyField(part.name) && !part.value.empty()) ||
            hasSecret(part.value)) {
            return "a credential in the request body";
        }
    }
    if (request.transport.clientCertPath && !request.transport.clientCertPath->empty()) {
        return "a client certificate";
    }
    return std::nullopt;
}

std::optional<ReqloomError> HostGate::checkCandidate(const std::string& host,
                                                     const SendPurpose& purpose) const {
    // Every actor whose values the request uses limits where it may go.
    for (const Actor* actor : purpose.actors) {
        if (auto refused = checkActorCandidate(host, *actor)) {
            return refused;
        }
    }
    return std::nullopt;
}

std::optional<ReqloomError> HostGate::checkActorCandidate(const std::string& host,
                                                          const Actor& actor) const {
    if (!actor.allowedHosts.empty()) {
        if (std::ranges::any_of(actor.allowedHosts, [&host](const std::string& entry) {
                return allowedHostMatches(entry, host);
            })) {
            return std::nullopt;
        }
        return refusal("credentials were not sent to " + host + ": " + actorLabel(actor) +
                       " may send its credentials only to the hosts in its allowed_hosts (" +
                       joined(actor.allowedHosts) +
                       "). If this host is meant to receive them, add it to allowed_hosts; it "
                       "will still need your approval.");
    }
    // The candidate is a host and port; the scheme is the approval's business.
    const auto withoutScheme = [](std::string_view key) {
        const auto sep = key.find("://");
        return sep == std::string_view::npos ? key : key.substr(sep + 3);
    };
    if (!baseHost_.empty() && withoutScheme(host) == withoutScheme(baseHost_)) {
        return std::nullopt;
    }
    return refusal(
        "credentials were not sent to " + host + ": " + actorLabel(actor) +
        " declares no allowed_hosts, so its credentials may go only to the host of the "
        "environment's baseUrl (" +
        (baseHost_.empty() ? std::string{"none is set"} : baseHost_) +
        "). If this host is meant to receive them, list every host the actor uses under its "
        "allowed_hosts; each will still need your approval.");
}

std::optional<ReqloomError> HostGate::checkApproval(const std::string& host,
                                                    const HttpRequest& request,
                                                    const std::string& credential) {
    const std::scoped_lock lock{mutex_};
    if (const auto it = decided_.find(host); it != decided_.end()) {
        if (it->second) {
            return std::nullopt;
        }
        return refusals_.at(host);
    }
    if (std::ranges::find(config_.approvedForRun, host) != config_.approvedForRun.end()) {
        decided_[host] = true;
        return std::nullopt;
    }
    const Project& project = *config_.project;
    const std::string projectLabel =
        "project '" + project.name + "'" + (project.id.empty() ? "" : " (id " + project.id + ")");
    if (config_.store != nullptr) {
        auto approved = config_.store->isApproved(project, host);
        if (!approved) {
            // A copy claiming another project's id, a project without an id, an unreadable store:
            // never send, never ask.
            return refusal("credentials were not sent to " + host + ": " + approved.error().detail);
        }
        if (*approved) {
            decided_[host] = true;
            return std::nullopt;
        }
    }
    HostApprovalRequest question{
        project.name,
        project.id,
        host,
        config_.registry != nullptr ? config_.registry->redact(request.url) : request.url,
        credential};
    const std::string notApproved = "credentials were not sent: " + host + " is not approved for " +
                                    projectLabel + " to receive " + credential + ".";
    if (config_.approver == nullptr) {
        auto error = refusal(notApproved +
                             " Approve it in the Reqloom app, or run this project once at a "
                             "terminal and answer yes.");
        refusals_.emplace(host, error);
        decided_[host] = false;
        return error;
    }
    switch (config_.approver->ask(question)) {
        case HostApprovalAnswer::Approve:
            if (config_.store != nullptr) {
                // The person said yes: send even when the answer cannot be recorded (the next run
                // asks again).
                (void)config_.store->approve(project, host);
            }
            decided_[host] = true;
            return std::nullopt;
        case HostApprovalAnswer::Deny: {
            auto error = refusal("credentials were not sent: you declined to send " + credential +
                                 " to " + host + " for " + projectLabel + ".");
            refusals_.emplace(host, error);
            decided_[host] = false;
            return error;
        }
        case HostApprovalAnswer::Unavailable:
            break;
    }
    auto error = refusal(notApproved + " " + config_.approver->howToApprove(question));
    refusals_.emplace(host, error);
    decided_[host] = false;
    return error;
}

std::optional<ReqloomError> HostGate::admit(HttpRequest& request, const SendPurpose& purpose) {
    if (config_.project == nullptr) {
        return refusal("credentials were not sent: no project to check the host against");
    }
    // A hop keeps the credential status of the request it continues while it still carries
    // anything the project or a hook supplied (`carriesSuppliedData`): a transformed secret in a
    // custom header or the body is not recognisable by its bytes, and an open redirect on an
    // approved host must not carry it elsewhere. A hop carrying none of that (a plain GET after a
    // 302, once Authorization and Cookie are dropped) is judged by its bytes, so ordinary
    // download redirects keep working. Either way the actors' limits apply. The callback stays on
    // the hop request (a copy), so it only checks and never replaces itself while it runs.
    const auto original = credentialIn(request, purpose);
    request.onRedirect =
        [this, actors = purpose.actors, original, query = std::string{queryOf(request.url)}](
            HttpRequest& next) -> std::expected<void, ReqloomError> {
        SendPurpose hop{actors, false, {}};
        if (original && carriesSuppliedData(next, query)) {
            hop.carriesCredential = true;
            hop.reason = *original + " (carried on through a redirect)";
        }
        if (auto refused = check(next, hop)) {
            return std::unexpected(std::move(*refused));
        }
        return {};
    };
    return check(request, purpose);
}

std::optional<ReqloomError> HostGate::check(const HttpRequest& request,
                                            const SendPurpose& purpose) {
    const auto credential = credentialIn(request, purpose);
    if (!credential) {
        return std::nullopt;
    }
    auto host = hostKeyForUrl(request.url);
    if (!host) {
        return refusal("credentials were not sent: " + host.error().detail);
    }
    if (auto refused = checkCandidate(*host, purpose)) {
        return refused;
    }
    if (auto refused = checkApproval(*host, request, *credential)) {
        return refused;
    }
    // Unverified TLS, or a certificate authority the project names, lets someone in between read
    // the credential: that is its own question, asked even for an approved host.
    if (std::string_view{*host}.starts_with("https://")) {
        std::string caveat;
        if (!request.transport.tlsVerify || !request.transport.tlsVerifyHost) {
            caveat = " with TLS verification off";
        } else if (request.transport.caBundlePath && !request.transport.caBundlePath->empty()) {
            caveat = " trusting the certificate authority in " + *request.transport.caBundlePath;
        }
        if (!caveat.empty()) {
            if (auto refused = checkApproval(*host + caveat, request, *credential)) {
                return refused;
            }
        }
    }
    // A proxy named by the project sees a plain-http request whole, and an https one too when the
    // project also turns TLS checks off or names its own CA: it needs approval as well.
    if (request.transport.proxy && !request.transport.proxy->empty()) {
        const auto& proxy = *request.transport.proxy;
        auto proxyHost =
            hostKeyForUrl(proxy.find("://") == std::string::npos ? "http://" + proxy : proxy);
        if (!proxyHost) {
            return refusal("credentials were not sent through the proxy: " +
                           proxyHost.error().detail);
        }
        if (auto refused = checkApproval(*proxyHost, request, *credential + " (via this proxy)")) {
            return refused;
        }
    }
    return std::nullopt;
}

std::expected<HttpResponse, ReqloomError> GatedHttpClient::send(const HttpRequest& request) {
    if (gate_ == nullptr) {
        return std::unexpected(refusal("no host approval check is configured; nothing was sent"));
    }
    HttpRequest checked = request;
    if (auto refused = gate_->admit(checked, purpose_)) {
        return std::unexpected(std::move(*refused));
    }
    return inner_.send(checked);
}

}  // namespace reqloom::engine
