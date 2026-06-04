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
