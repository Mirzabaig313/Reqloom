find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

# Renderer unit tests — exercise the three CLI output formats against
# hand-crafted RunResult fixtures. No I/O, no engine, no network.
add_executable(reqloom-cli-renderer-tests
    unit/RendererTest.cpp
    unit/JsonContractTest.cpp
)
# Qt6::Core parses the rendered JSON and XML to check that they are well formed.
target_link_libraries(reqloom-cli-renderer-tests
    PRIVATE
        reqloom-cli-renderers
        reqloom::engine
        Qt6::Core
        GTest::gtest
        GTest::gtest_main
)
reqloom_set_warnings(reqloom-cli-renderer-tests)
reqloom_apply_sanitizers(reqloom-cli-renderer-tests)
reqloom_isolate_test_config(reqloom-cli-renderer-tests)

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
        reqloom-cli-support
        reqloom::engine
        Qt6::Core
        GTest::gtest
        GTest::gtest_main
)
reqloom_set_warnings(reqloom-cli-import-tests)
reqloom_apply_sanitizers(reqloom-cli-import-tests)
reqloom_isolate_test_config(reqloom-cli-import-tests)

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

# Run command tests — compile the command directly, like the import tests.
add_executable(reqloom-cli-run-tests
    unit/RunCommandTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/commands/RunCommand.cpp
)
target_include_directories(reqloom-cli-run-tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_link_libraries(reqloom-cli-run-tests
    PRIVATE
        reqloom-cli-renderers
        reqloom-cli-support
        reqloom::engine
        Qt6::Core
        GTest::gtest
        GTest::gtest_main
)
reqloom_set_warnings(reqloom-cli-run-tests)
reqloom_apply_sanitizers(reqloom-cli-run-tests)
reqloom_isolate_test_config(reqloom-cli-run-tests)

reqloom_forbid_dependencies(reqloom-cli-run-tests
    Qt6::Widgets
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickWidgets
)

gtest_discover_tests(reqloom-cli-run-tests
    PROPERTIES LABELS "cli_unit"
    DISCOVERY_TIMEOUT 60
)

# Agent-facing output, end to end: `reqloom run` against the mock server, checking what reaches
# stdout and stderr in every format.
# Needs the mock server, which is built only with the engine tests.
if(TARGET reqloom-mock-sut)
    add_executable(reqloom-cli-agent-output-tests
        unit/AgentOutputTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/commands/RunCommand.cpp
        ${CMAKE_SOURCE_DIR}/engine/tests/integration/MockSutHarness.cpp
    )
    target_include_directories(reqloom-cli-agent-output-tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../src
            ${CMAKE_SOURCE_DIR}/engine/tests/integration
    )
    target_compile_definitions(reqloom-cli-agent-output-tests
        PRIVATE REQLOOM_MOCK_SUT_PATH="$<TARGET_FILE:reqloom-mock-sut>"
    )
    add_dependencies(reqloom-cli-agent-output-tests reqloom-mock-sut)
    target_link_libraries(reqloom-cli-agent-output-tests
        PRIVATE
            reqloom-cli-renderers
            reqloom-cli-support
            reqloom::engine
            Qt6::Core
            GTest::gtest
            GTest::gtest_main
    )
    reqloom_set_warnings(reqloom-cli-agent-output-tests)
    reqloom_apply_sanitizers(reqloom-cli-agent-output-tests)
    reqloom_isolate_test_config(reqloom-cli-agent-output-tests)

    reqloom_forbid_dependencies(reqloom-cli-agent-output-tests
        Qt6::Widgets
        Qt6::Gui
        Qt6::Quick
        Qt6::QuickWidgets
    )

    gtest_discover_tests(reqloom-cli-agent-output-tests
        PROPERTIES LABELS "cli_unit"
        DISCOVERY_TIMEOUT 60
    )
endif()

# Lint command tests — compile the command directly, like the import tests.
add_executable(reqloom-cli-lint-tests
    unit/LintCommandTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/commands/LintCommand.cpp
)
target_include_directories(reqloom-cli-lint-tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_link_libraries(reqloom-cli-lint-tests
    PRIVATE
        reqloom-cli-renderers
        reqloom-cli-support
        reqloom::engine
        Qt6::Core
        GTest::gtest
        GTest::gtest_main
)
reqloom_set_warnings(reqloom-cli-lint-tests)
reqloom_apply_sanitizers(reqloom-cli-lint-tests)
reqloom_isolate_test_config(reqloom-cli-lint-tests)

reqloom_forbid_dependencies(reqloom-cli-lint-tests
    Qt6::Widgets
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickWidgets
)

gtest_discover_tests(reqloom-cli-lint-tests
    PROPERTIES LABELS "cli_unit"
    DISCOVERY_TIMEOUT 60
)

# Argument parser, report writer and signal handling tests.
add_executable(reqloom-cli-support-tests
    unit/ArgParserTest.cpp
    unit/ReportWriterTest.cpp
    unit/RunInterruptTest.cpp
)
target_include_directories(reqloom-cli-support-tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_link_libraries(reqloom-cli-support-tests
    PRIVATE
        reqloom-cli-support
        reqloom::engine
        Qt6::Core
        GTest::gtest
        GTest::gtest_main
)
reqloom_set_warnings(reqloom-cli-support-tests)
reqloom_apply_sanitizers(reqloom-cli-support-tests)
reqloom_isolate_test_config(reqloom-cli-support-tests)

reqloom_forbid_dependencies(reqloom-cli-support-tests
    Qt6::Widgets
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickWidgets
)

gtest_discover_tests(reqloom-cli-support-tests
    PROPERTIES LABELS "cli_unit"
    DISCOVERY_TIMEOUT 60
)
