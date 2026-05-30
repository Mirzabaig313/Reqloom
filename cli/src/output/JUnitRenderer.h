// JUnit XML renderer for `chainapi run --format junit`.
//
// Output mirrors the surface JUnit consumers (Jenkins, GitLab, Buildkite)
// expect: one `<testsuites>` wrapping one `<testsuite>` per chain, with
// one `<testcase>` per step. Failed steps emit `<failure>`, skipped or
// cached steps emit `<skipped>`, blocked / cancelled steps emit
// `<error>`. Poll-attempt rows are folded into their parent testcase as
// `<system-out>` so the suite count matches the number of declared
// operations rather than physical step rows.

#pragma once

#include <chainapi/engine/PublicApi.h>

#include <ostream>
#include <string_view>

namespace chainapi::cli {

class JUnitRenderer {
public:
    explicit JUnitRenderer(std::ostream& out);

    void render(const engine::OperationId& target,
                std::string_view environment,
                const engine::RunResult& result);

private:
    std::ostream& out_;
};

}  // namespace chainapi::cli
