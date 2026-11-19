// SecretReveal — which stored secrets the person has asked to see, and their values while shown
// (desktop only). A value is read from the keychain when the person asks, kept only while it is
// shown, and forgotten when hidden or when the view closes. Nothing here writes anything: no
// history, log, saved example, clipboard or event receives a value.
#pragma once

#include "SecretManager.h"

#include <QtCore/QHash>
#include <QtCore/QString>

namespace reqloom::desktop {

class ProjectModel;

/// What the person sees for one secret.
struct RevealState {
    bool shown{false};
    QString value;    ///< The stored value while shown; empty otherwise.
    QString message;  ///< Why nothing is shown (not set, read failed, folder refused).
};

class SecretReveal {
public:
    explicit SecretReveal(const SecretManager& manager) : manager_(manager) {}

    /// Read `name` now, keeping nothing: for a one-off view (the request panel).
    [[nodiscard]] RevealState read(const ProjectModel& project, const QString& name) const;

    /// Show `name` (reading it now) or, when shown, hide it and forget the value.
    const RevealState& toggle(const ProjectModel& project, const QString& name);

    /// The state of `name`, or null when nothing was asked for since the last `hideAll`.
    [[nodiscard]] const RevealState* state(const QString& name) const;

    /// Hide and forget everything (the dialog closed, the project changed).
    void hideAll();

private:
    const SecretManager& manager_;
    QHash<QString, RevealState> states_;
};

}  // namespace reqloom::desktop
