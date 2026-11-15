// `reqloom import`: import an API definition or client export into a Reqloom project.
#pragma once

#include <QtCore/QStringList>

namespace reqloom::engine {
class ProjectTrustStore;
class SecretStore;
}  // namespace reqloom::engine

namespace reqloom::cli {

/// Import a spec or collection into a Reqloom project, moving its credentials into the
/// OS keychain's secret store (bound to the project folder by the user's trust store).
[[nodiscard]] int importCommand(const QStringList& args);

/// `importCommand` with the secret store and trust store supplied by the caller.
/// Either may be null; credentials then stay in the YAML with a warning.
[[nodiscard]] int importCommand(const QStringList& args,
                                engine::SecretStore* store,
                                engine::ProjectTrustStore* trust);

}  // namespace reqloom::cli
