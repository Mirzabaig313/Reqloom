# cli/CMakeLists.txt
#
# Builds the reqloom binary. Links libreqloom-engine + Qt6::Core only.
# Renderers are split into a small OBJECT library so the test target can
# exercise them without re-listing every source.

# ─── Renderers (text / JSON / JUnit) — reusable in tests ──────────────────────
add_library(reqloom-cli-renderers OBJECT
    src/output/TextRenderer.cpp
    src/output/JsonRenderer.cpp
    src/output/NextAction.cpp
    src/output/JUnitRenderer.cpp
    src/output/TextEncoding.cpp
)
target_include_directories(reqloom-cli-renderers
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(reqloom-cli-renderers
    PUBLIC reqloom::engine
)
reqloom_set_warnings(reqloom-cli-renderers)
reqloom_apply_sanitizers(reqloom-cli-renderers)

# Argument parsing, report writing and signal handling shared by the commands and their tests.
add_library(reqloom-cli-support OBJECT
    src/commands/ArgParser.cpp
    src/commands/RunInterrupt.cpp
    src/output/ReportWriter.cpp
)
target_include_directories(reqloom-cli-support
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
)
# Qt6::Core for QStringList arguments and QSaveFile's write-then-rename.
target_link_libraries(reqloom-cli-support
    PUBLIC
        reqloom::engine
        Qt6::Core
)
reqloom_set_warnings(reqloom-cli-support)
reqloom_apply_sanitizers(reqloom-cli-support)


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
        reqloom-cli-support
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
