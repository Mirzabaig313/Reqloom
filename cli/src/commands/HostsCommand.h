// `reqloom hosts`: list, approve and revoke the hosts a project may send credentials to
// (PRD FR-15.9), and turn approval from the command line off.
#pragma once

#include <QtCore/QStringList>

#include <cstdio>
#include <functional>

namespace reqloom::cli {

/// Exit code per `ExitCodes.h`. `terminal` says whether a person is at the keyboard; approving
/// needs one and asks a yes/no question on `out`, read from `in` (tests pass fakes).
[[nodiscard]] int hostsCommand(const QStringList& args,
                               const std::function<bool()>& terminal,
                               std::FILE* in,
                               std::FILE* out);

/// `hostsCommand` with the real terminal check.
[[nodiscard]] int hostsCommand(const QStringList& args);

}  // namespace reqloom::cli
