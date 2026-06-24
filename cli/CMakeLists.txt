# cli/CMakeLists.txt
#
# Builds the reqloom binary. Links libreqloom-engine + Qt6::Core only.
# Renderers are split into a small OBJECT library so the test target can
# exercise them without re-listing every source.

# ─── Renderers (text / JSON / JUnit) — reusable in tests ──────────────────────
add_library(reqloom-cli-renderers OBJECT
    src/output/TextRenderer.cpp
    src/output/JsonRenderer.cpp
    src/output/JUnitRenderer.cpp
)
target_include_directories(reqloom-cli-renderers
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(reqloom-cli-renderers
    PUBLIC reqloom::engine
)
reqloom_set_warnings(reqloom-cli-renderers)
reqloom_apply_sanitizers(reqloom-cli-renderers)


# ─── reqloom binary ─────────────────────────────────────────────────────────
add_executable(reqloom-cli
    src/main.cpp
    src/commands/RunCommand.cpp
    src/commands/LintCommand.cpp
    src/commands/ImportCommand.cpp
)

target_link_libraries(reqloom-cli
    PRIVATE
        reqloom-cli-renderers
        reqloom::engine
        Qt6::Core
)

set_target_properties(reqloom-cli PROPERTIES OUTPUT_NAME "reqloom")

reqloom_set_warnings(reqloom-cli)
reqloom_apply_sanitizers(reqloom-cli)

# CLI must not pull in Qt UI libraries even transitively.
reqloom_forbid_dependencies(reqloom-cli
    Qt6::Widgets
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickWidgets
)


# ─── Tests ───────────────────────────────────────────────────────────────────
if(REQLOOM_BUILD_TESTS)
    add_subdirectory(tests)
endif()
