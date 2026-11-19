// SecretReveal — see header.
#include "SecretReveal.h"

namespace reqloom::desktop {

namespace {

/// Overwrite a value's characters before dropping it (best effort: Qt may hold other copies).
void wipe(QString& value) {
    value.fill(QLatin1Char('\0'));
    value.clear();
}

}  // namespace

RevealState SecretReveal::read(const ProjectModel& project, const QString& name) const {
    RevealState state;
    QString error;
    switch (manager_.reveal(project, name, state.value, error)) {
        case SecretManager::RevealOutcome::Revealed:
            state.shown = true;
            break;
        case SecretManager::RevealOutcome::NotSet:
            state.message = QStringLiteral("No value is stored for this secret.");
            break;
        case SecretManager::RevealOutcome::Failed:
            state.message = QStringLiteral("Couldn't read the stored value: %1").arg(error);
            break;
    }
    return state;
}

const RevealState& SecretReveal::toggle(const ProjectModel& project, const QString& name) {
    auto& state = states_[name];
    if (state.shown) {
        wipe(state.value);
        state = RevealState{};
        return state;
    }
    state = read(project, name);
    return state;
}

const RevealState* SecretReveal::state(const QString& name) const {
    const auto it = states_.constFind(name);
    return it == states_.cend() ? nullptr : &*it;
}

void SecretReveal::hideAll() {
    for (auto& state : states_) {
        wipe(state.value);
    }
    states_.clear();
}

}  // namespace reqloom::desktop
