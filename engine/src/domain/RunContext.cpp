#include <reqloom/engine/RunContext.h>

#include "SecretRegistry.h"

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <string_view>
#include <vector>

namespace reqloom::engine {

namespace {

/// Session variables that describe a credential rather than being one.
/// Registering them would redact ordinary words ("Bearer", "us-east-1").
constexpr std::array<std::string_view, 6> kSessionMetadataKeys = {
    "token_type", "expires_in", "scope", "region", "service", "realm"};

void registerSessionSecrets(SecretRegistry& registry, const ActorSession& session) {
    for (const auto& [name, value] : session.variables) {
        if (std::ranges::find(kSessionMetadataKeys, std::string_view{name}) ==
            kSessionMetadataKeys.end()) {
            registry.add(value);
        }
    }
    for (const auto& [name, value] : session.injectHeaders) {
        registry.add(value);
    }
    for (const auto& [name, value] : session.injectQueryParams) {
        registry.add(value);
    }
    if (session.transport && session.transport->clientKeyPassword) {
        registry.add(*session.transport->clientKeyPassword);
    }
}

}  // namespace

struct RunContext::Impl {
    std::map<ActorId, ActorSession> sessions;
    /// Per-actor cookie jar, oldest first.
    std::map<ActorId, std::vector<JarCookie>> cookieJars;
    std::map<ResourceId, std::vector<ResourceInstance>> instances;
    std::map<ResourceId, std::size_t> iterations;
    std::vector<ResourceInstance> emptyInstances;
    std::vector<StepResult> steps;
    std::vector<ExtractionTrace> extractionTrace;
    /// Heap-held: the registry owns a mutex and is not movable.
    std::unique_ptr<SecretRegistry> secretRegistry{std::make_unique<SecretRegistry>()};
};

RunContext::RunContext() : impl_(std::make_unique<Impl>()) {}
RunContext::~RunContext() = default;
RunContext::RunContext(RunContext&&) noexcept = default;
RunContext& RunContext::operator=(RunContext&&) noexcept = default;

const ActorSession* RunContext::session(const ActorId& actor) const noexcept {
    const auto it = impl_->sessions.find(actor);
    return it == impl_->sessions.end() ? nullptr : &it->second;
}

void RunContext::putSession(const ActorId& actor, ActorSession session) {
    registerSessionSecrets(*impl_->secretRegistry, session);
    impl_->sessions[actor] = std::move(session);
}

void RunContext::invalidateSession(const ActorId& actor) {
    impl_->sessions.erase(actor);
    // Cookies travel with the session in 401-recovery scenarios — the
    // pre-recovery jar reflected the now-stale token, so dropping it
    // alongside the session prevents the retry from sending bad cookies.
    impl_->cookieJars.erase(actor);
}

void RunContext::clearSessions() {
    impl_->sessions.clear();
    impl_->cookieJars.clear();
}

std::map<std::string, std::string> RunContext::cookies(const ActorId& actor) const {
    std::map<std::string, std::string> out;
    if (const auto it = impl_->cookieJars.find(actor); it != impl_->cookieJars.end()) {
        for (const auto& cookie : it->second) {
            out[cookie.name] = cookie.value;
        }
    }
    return out;
}

std::vector<JarCookie> RunContext::jarCookies(const ActorId& actor) const {
    const auto it = impl_->cookieJars.find(actor);
    return it == impl_->cookieJars.end() ? std::vector<JarCookie>{} : it->second;
}

void RunContext::setCookie(const ActorId& actor, std::string name, std::string value) {
    storeCookie(actor, JarCookie{.name = std::move(name), .value = std::move(value)});
}

void RunContext::storeCookie(const ActorId& actor, JarCookie cookie) {
    if (cookie.name.empty()) {
        return;
    }
    impl_->secretRegistry->add(cookie.value);
    auto& jar = impl_->cookieJars[actor];
    std::erase_if(jar, [&](const JarCookie& held) {
        return held.name == cookie.name && held.domain.has_value() == cookie.domain.has_value() &&
               held.domain.value_or(held.setterHost) == cookie.domain.value_or(cookie.setterHost) &&
               held.path == cookie.path;
    });
    jar.push_back(std::move(cookie));
}

void RunContext::removeCookie(const ActorId& actor, const std::string& name) {
    if (const auto it = impl_->cookieJars.find(actor); it != impl_->cookieJars.end()) {
        std::erase_if(it->second, [&](const JarCookie& held) { return held.name == name; });
    }
}

void RunContext::removeCookie(const ActorId& actor,
                              const std::string& name,
                              const std::string_view host) {
    const auto it = impl_->cookieJars.find(actor);
    if (it == impl_->cookieJars.end()) {
        return;
    }
    std::erase_if(it->second, [&](const JarCookie& held) {
        if (held.name != name) {
            return false;
        }
        if (held.setterHost.empty() || held.setterHost == host) {
            return true;
        }
        if (!held.domain) {
            return false;
        }
        const std::string_view domain{*held.domain};
        return host == domain || (host.size() > domain.size() && host.ends_with(domain) &&
                                  host[host.size() - domain.size() - 1] == '.');
    });
}

void RunContext::clearCookies(const ActorId& actor) {
    impl_->cookieJars.erase(actor);
}

const std::vector<ResourceInstance>& RunContext::instances(
    const ResourceId& resource) const noexcept {
    const auto it = impl_->instances.find(resource);
    return it == impl_->instances.end() ? impl_->emptyInstances : it->second;
}

void RunContext::appendInstance(const ResourceId& resource, ResourceInstance instance) {
    impl_->instances[resource].push_back(std::move(instance));
}

void RunContext::replaceInstances(const ResourceId& resource,
                                  const OperationId& producer,
                                  std::vector<ResourceInstance> instances) {
    auto& stored = impl_->instances[resource];
    std::erase_if(stored,
                  [&](const ResourceInstance& instance) { return instance.producer == producer; });
    for (auto& instance : instances) {
        instance.producer = producer;
        stored.push_back(std::move(instance));
    }
}

void RunContext::clearExtractions() {
    impl_->instances.clear();
}

void RunContext::setIteration(const ResourceId& resource, std::optional<std::size_t> index) {
    if (index.has_value()) {
        impl_->iterations[resource] = *index;
    } else {
        impl_->iterations.erase(resource);
    }
}

std::optional<std::size_t> RunContext::iteration(const ResourceId& resource) const noexcept {
    const auto it = impl_->iterations.find(resource);
    if (it == impl_->iterations.end()) {
        return std::nullopt;
    }
    return it->second;
}

void RunContext::record(StepResult step) {
    impl_->steps.push_back(std::move(step));
}

const std::vector<StepResult>& RunContext::steps() const noexcept {
    return impl_->steps;
}

void RunContext::recordExtraction(ExtractionTrace trace) {
    impl_->extractionTrace.push_back(std::move(trace));
}

const std::vector<ExtractionTrace>& RunContext::extractionTrace() const noexcept {
    return impl_->extractionTrace;
}

SecretRegistry& RunContext::secretRegistry() const noexcept {
    return *impl_->secretRegistry;
}

}  // namespace reqloom::engine
