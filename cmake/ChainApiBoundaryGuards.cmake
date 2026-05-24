# ChainApiBoundaryGuards.cmake
#
# Mechanically enforces the architectural guardrails from PRD §8.6 and
# Project Layout §4: the engine and CLI MUST NOT depend on Qt UI
# libraries (Widgets, Gui, Quick, QuickWidgets) or QScintilla.
#
# Two public functions:
#
#   chainapi_forbid_dependencies(<target> <forbidden_lib1> ...)
#       Records a list of forbidden libraries for the target.
#
#   chainapi_enforce_boundary_rules()
#       Walks each registered target's transitive link interface and
#       fails the configure step if any forbidden library appears.
#
# Both direct LINK_LIBRARIES and INTERFACE_LINK_LIBRARIES are walked.
# Header-include leaks (where a file `#include`s a forbidden header
# without linking it) are caught separately by the CI grep job in
# `.github/workflows/boundary-check.yml`.

set_property(GLOBAL PROPERTY CHAINAPI_BOUNDARY_TARGETS "")

function(chainapi_forbid_dependencies target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR
            "chainapi_forbid_dependencies: '${target}' is not a target")
    endif()

    set(_forbidden ${ARGN})
    set_property(TARGET ${target}
        PROPERTY CHAINAPI_FORBIDDEN_DEPS "${_forbidden}")

    get_property(_registered GLOBAL PROPERTY CHAINAPI_BOUNDARY_TARGETS)
    list(APPEND _registered ${target})
    set_property(GLOBAL PROPERTY CHAINAPI_BOUNDARY_TARGETS "${_registered}")
endfunction()


# Recursively gather every link library reachable from `target`.
# Result is appended to the parent-scope variable `${out_var}`.
function(_chainapi_collect_link_deps target out_var)
    if(NOT TARGET ${target})
        return()
    endif()

    get_property(_visited GLOBAL PROPERTY CHAINAPI_VISITED_${target})
    if(_visited)
        return()
    endif()
    set_property(GLOBAL PROPERTY CHAINAPI_VISITED_${target} TRUE)

    set(_deps "")
    foreach(_prop LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
        get_target_property(_v ${target} ${_prop})
        if(_v)
            list(APPEND _deps ${_v})
        endif()
    endforeach()

    set(_collected "")
    foreach(_d IN LISTS _deps)
        # Skip generator expressions and nonsense entries
        if(_d MATCHES "^\\$<")
            continue()
        endif()
        if(_d STREQUAL "_d-NOTFOUND")
            continue()
        endif()
        list(APPEND _collected ${_d})
        if(TARGET ${_d})
            _chainapi_collect_link_deps(${_d} _collected)
        endif()
    endforeach()

    set(${out_var} "${${out_var}};${_collected}" PARENT_SCOPE)
endfunction()


function(chainapi_enforce_boundary_rules)
    get_property(_targets GLOBAL PROPERTY CHAINAPI_BOUNDARY_TARGETS)

    if(NOT _targets)
        message(STATUS "[ChainAPI boundary] No targets registered; skipping.")
        return()
    endif()

    set(_any_violation FALSE)

    foreach(_target IN LISTS _targets)
        if(NOT TARGET ${_target})
            continue()
        endif()

        get_target_property(_forbidden ${_target} CHAINAPI_FORBIDDEN_DEPS)
        if(NOT _forbidden)
            continue()
        endif()

        # Reset visited markers per top-level target. The visited markers
        # are global properties named CHAINAPI_VISITED_<target>; they are
        # set during _chainapi_collect_link_deps and persist across calls
        # within a single CMake configure pass. Because we only call
        # chainapi_enforce_boundary_rules() once at the end of the root
        # CMakeLists.txt, sharing the markers across sibling top-level
        # targets is safe — each target rooted at a different node walks
        # its own subgraph.
        set(_collected "")
        _chainapi_collect_link_deps(${_target} _collected)

        # De-dup
        if(_collected)
            list(REMOVE_DUPLICATES _collected)
        endif()

        foreach(_forbid IN LISTS _forbidden)
            foreach(_dep IN LISTS _collected)
                if(_dep STREQUAL _forbid)
                    message(SEND_ERROR
                        "[ChainAPI boundary VIOLATION] target '${_target}' "
                        "depends on '${_forbid}' (transitively or directly). "
                        "See PRD §8.6 / Project Layout §4. Engine and CLI "
                        "must not link Qt UI libraries.")
                    set(_any_violation TRUE)
                endif()
            endforeach()
        endforeach()
    endforeach()

    if(_any_violation)
        message(FATAL_ERROR
            "[ChainAPI boundary] One or more architectural violations were "
            "detected. Fix the offending target(s) before continuing.")
    else()
        message(STATUS "[ChainAPI boundary] All targets pass dependency rules.")
    endif()
endfunction()
