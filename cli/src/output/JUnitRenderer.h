// JUnit XML renderer for `reqloom run --format junit`: one `<testsuite>` per chain, one
// `<testcase>` per step; poll-attempt rows fold into their parent's `<system-out>`.

#pragma once

#include <reqloom/engine/PublicApi.h>

#include <ostream>
#include <string_view>

namespace reqloom::cli {

class JUnitRenderer {
public:
    explicit JUnitRenderer(std::ostream& out);

    void render(const engine::OperationId& target,
                std::string_view environment,
                const engine::RunResult& result);

private:
    std::ostream& out_;
};

}  // namespace reqloom::cli
