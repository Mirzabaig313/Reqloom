// Resource — a domain entity exposed via API.
#pragma once

#include <reqloom/engine/Operation.h>
#include <map>
#include <string>

namespace reqloom::engine {

struct Resource {
    ResourceId id;
    std::string description;

    /// Operation map keyed by operation short name (e.g. "create", "publish").
    /// The fully qualified id is "<resource>.<op_name>".
    std::map<std::string, Operation> operations;

    /// Where the resource was defined; see `SourceLocation`.
    SourceLocation source;
};

}  // namespace reqloom::engine
