// Resource — a domain entity exposed via API.
#pragma once

#include <chainapi/engine/Operation.h>
#include <map>
#include <string>

namespace chainapi::engine {

struct Resource {
    ResourceId id;
    std::string description;

    /// Operation map keyed by operation short name (e.g. "create", "publish").
    /// The fully qualified id is "<resource>.<op_name>".
    std::map<std::string, Operation> operations;
};

}  // namespace chainapi::engine
