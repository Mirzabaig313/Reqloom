#include <chainapi/engine/RunContext.h>

#include <map>
#include <vector>

namespace chainapi::engine {

struct RunContext::Impl {
    std::map<ActorId, ActorSession> sessions;
    std::map<ResourceId, std::vector<ResourceInstance>> instances;
    std::vector<ResourceInstance> emptyInstances;
    std::vector<StepResult> steps;
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
}

const std::vector<ResourceInstance>&
RunContext::instances(const ResourceId& resource) const noexcept {
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

}  // namespace chainapi::engine
