// Variable suggestions — the set of `{{...}}` references usable at a given point
// in a chain (upstream extracts, the actor's session tokens, env vars, declared
// secrets, and `$.` built-ins). Powers `{{` autocomplete in the editors so a
// user never has to memorise or mistype a variable name.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace reqloom::engine {

/// One suggestion. `token` is the reference body — the UI inserts it as
/// `{{token}}`. `kind` lets the UI group/icon them; `detail` is a short human
/// hint (producing operation id, actor id, environment name, or a description
/// for built-ins).
struct VariableSuggestion {
    enum class Kind : std::uint8_t { Extract, ActorToken, EnvVar, Secret, Builtin };

    std::string token{};
    Kind kind{Kind::Extract};
    std::string detail{};
};

}  // namespace reqloom::engine
