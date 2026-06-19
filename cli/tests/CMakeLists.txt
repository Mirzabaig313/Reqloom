find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

# Renderer unit tests — exercise the three CLI output formats against
# hand-crafted RunResult fixtures. No I/O, no engine, no network.
add_executable(reqloom-cli-renderer-tests
    unit/RendererTest.cpp
)
target_link_libraries(reqloom-cli-renderer-tests
    PRIVATE
        reqloom-cli-renderers
        reqloom::engine
        GTest::gtest
        GTest::gtest_main
)
reqloom_set_warnings(reqloom-cli-renderer-tests)
reqloom_apply_sanitizers(reqloom-cli-renderer-tests)

# CLI test binary follows the same boundary as the CLI itself.
reqloom_forbid_dependencies(reqloom-cli-renderer-tests
    Qt6::Widgets
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickWidgets
)

gtest_discover_tests(reqloom-cli-renderer-tests
    PROPERTIES LABELS "cli_unit"
    DISCOVERY_TIMEOUT 60
)

# Import command tests — compile the command directly (commands aren't a
# shared object library) and drive it against temp specs + output dirs.
add_executable(reqloom-cli-import-tests
    unit/ImportCommandTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/commands/ImportCommand.cpp
)
target_include_directories(reqloom-cli-import-tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_link_libraries(reqloom-cli-import-tests
    PRIVATE
        reqloom::engine
        Qt6::Core
        GTest::gtest
        GTest::gtest_main
)
reqloom_set_warnings(reqloom-cli-import-tests)
reqloom_apply_sanitizers(reqloom-cli-import-tests)

reqloom_forbid_dependencies(reqloom-cli-import-tests
    Qt6::Widgets
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickWidgets
)

gtest_discover_tests(reqloom-cli-import-tests
    PROPERTIES LABELS "cli_unit"
    DISCOVERY_TIMEOUT 60
)
