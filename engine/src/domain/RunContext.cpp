#include <chainapi/engine/RunContext.h>

#include <map>
#include <vector>

namespace chainapi::engine {

struct RunContext::Impl {
    std::map<ActorId, ActorSession> sessions;
    /// Per-actor cookie jar. We intentionally treat the jar as simple
    /// name→value pairs, not as full RFC 6265 cookies with attributes.
    /// The engine isn't a browser; tracking domain / path / expiry
    /// would invite bugs without buying real testing power.
    std::map<ActorId, std::map<std::string, std::string>> cookieJars;
    std::map<ResourceId, std::vector<ResourceInstance>> instances;
    std::vector<ResourceInstance> emptyInstances;
    std::vector<StepResult> steps;
    std::vector<ExtractionTrace> extractionTrace;
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
    impl_->sessions[actor] = std::move(session);
}

void RunContext::invalidateSession(const ActorId& actor) {
    impl_->sessions.erase(actor);
    // Cookies travel with the session in 401-recovery scenarios — the
    // pre-recovery jar reflected the now-stale token, so dropping it
    // alongside the session prevents the retry from sending bad cookies.
    impl_->cookieJars.erase(actor);
}

std::map<std::string, std::string> RunContext::cookies(const ActorId& actor) const {
    const auto it = impl_->cookieJars.find(actor);
    if (it == impl_->cookieJars.end()) {
        return {};
    }
    return it->second;
}

void RunContext::setCookie(const ActorId& actor, std::string name, std::string value) {
    if (name.empty()) {
        return;
    }
    impl_->cookieJars[actor][std::move(name)] = std::move(value);
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

void RunContext::clearExtractions() {
    impl_->instances.clear();
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

}  // namespace chainapi::engine
