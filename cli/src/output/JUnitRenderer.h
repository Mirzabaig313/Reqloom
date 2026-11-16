// JUnit XML renderer for `reqloom run --format junit`: one `<testsuite>` per chain, one
// `<testcase>` per step; poll-attempt rows fold into their parent's `<system-out>`.

#pragma once

#include <reqloom/engine/PublicApi.h>

#include <optional>
#include <ostream>
#include <string_view>

namespace reqloom::cli {

class JUnitRenderer {
public:
    explicit JUnitRenderer(std::ostream& out);

    void render(const engine::OperationId& target,
                std::string_view environment,
                const engine::RunResult& result);

    /// A run that ended before its first step: one `<testcase>` carrying an `<error>` with the
    /// code as `type`, the detail's first line as `message` and the whole detail as its body.
    void renderError(const engine::OperationId& target,
                     std::string_view environment,
                     std::optional<engine::RunId> run,
                     std::string_view code,
                     std::string_view detail);

private:
    std::ostream& out_;
};

}  // namespace reqloom::cli
