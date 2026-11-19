// The read-only project commands an agent (or a person at a terminal) uses to understand a project
// without opening the desktop app (PRD FR-13.7–13.9): `reqloom list`, `reqloom describe <op>` and
// `reqloom plan <op>`. None of them sends a request, reads a secret value or writes a file; they
// print templates and names, never resolved values.
#pragma once

#include <QtCore/QStringList>

namespace reqloom::cli {

/// `reqloom list`: every actor, resource and operation. Exit code per `ExitCodes.h`.
[[nodiscard]] int listCommand(const QStringList& args);

/// `reqloom describe <resource.operation>`: one operation in full.
[[nodiscard]] int describeCommand(const QStringList& args);

/// `reqloom plan <resource.operation>`: the resolved chain and what running it needs.
[[nodiscard]] int planCommand(const QStringList& args);

}  // namespace reqloom::cli
