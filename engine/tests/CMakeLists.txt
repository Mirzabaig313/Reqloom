find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

# Mock SUT — used as a CTest fixture for integration tests.
add_subdirectory(fixtures/mock-sut)

# ─── Unit tests ──────────────────────────────────────────────────────────────
# Smoke test: confirms libchainapi-engine builds, links, and the public
# error-code mapping is stable.
add_executable(chainapi-engine-unit-tests
    unit/SmokeTest.cpp
)
target_link_libraries(chainapi-engine-unit-tests
    PRIVATE
        chainapi::engine
        GTest::gtest
        GTest::gtest_main
)
chainapi_set_warnings(chainapi-engine-unit-tests)
chainapi_apply_sanitizers(chainapi-engine-unit-tests)

gtest_discover_tests(chainapi-engine-unit-tests
    PROPERTIES LABELS "engine_unit"
)

# ─── Integration tests ───────────────────────────────────────────────────────
# Exercise the engine end-to-end against the mock SUT.
add_executable(chainapi-engine-integration-tests
    integration/MockSutHarness.h
    integration/MockSutHarness.cpp
    integration/HappyPathTest.cpp
)
target_link_libraries(chainapi-engine-integration-tests
    PRIVATE
        chainapi::engine
        GTest::gtest
        GTest::gtest_main
)
target_compile_definitions(chainapi-engine-integration-tests
    PRIVATE
        CHAINAPI_MOCK_SUT_PATH="$<TARGET_FILE:chainapi-mock-sut>"
        CHAINAPI_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
        CHAINAPI_SAMPLES_DIR="${CMAKE_SOURCE_DIR}/samples"
)
chainapi_set_warnings(chainapi-engine-integration-tests)
chainapi_apply_sanitizers(chainapi-engine-integration-tests)

gtest_discover_tests(chainapi-engine-integration-tests
    PROPERTIES LABELS "engine_integration"
)

# All test binaries must respect the same engine-side boundary.
foreach(_t
    chainapi-engine-unit-tests
    chainapi-engine-integration-tests
)
    chainapi_forbid_dependencies(${_t}
        Qt6::Widgets
        Qt6::Gui
        Qt6::Quick
        Qt6::QuickWidgets
    )
endforeach()
