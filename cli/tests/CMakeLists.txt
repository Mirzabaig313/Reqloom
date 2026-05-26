find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

# Renderer unit tests — exercise the three CLI output formats against
# hand-crafted RunResult fixtures. No I/O, no engine, no network.
add_executable(chainapi-cli-renderer-tests
    unit/RendererTest.cpp
)
target_link_libraries(chainapi-cli-renderer-tests
    PRIVATE
        chainapi-cli-renderers
        chainapi::engine
        GTest::gtest
        GTest::gtest_main
)
chainapi_set_warnings(chainapi-cli-renderer-tests)
chainapi_apply_sanitizers(chainapi-cli-renderer-tests)

# CLI test binary follows the same boundary as the CLI itself.
chainapi_forbid_dependencies(chainapi-cli-renderer-tests
    Qt6::Widgets
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickWidgets
)

gtest_discover_tests(chainapi-cli-renderer-tests
    PROPERTIES LABELS "cli_unit"
)
