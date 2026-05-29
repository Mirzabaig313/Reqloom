find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

# Mock SUT — used as a CTest fixture for integration tests.
add_subdirectory(fixtures/mock-sut)

# ─── Unit tests ──────────────────────────────────────────────────────────────
# Smoke test: confirms libchainapi-engine builds, links, and the public
# error-code mapping is stable.
# VariableResolverTest: pinned to the domain layer (PRD §5.7), needs the
# private `engine/src/` include path to reach `domain/VariableResolver.h`.
add_executable(chainapi-engine-unit-tests
    unit/SmokeTest.cpp
    unit/VariableResolverTest.cpp
    unit/PredicateEvaluatorTest.cpp
    unit/SchemaParserPollUntilTest.cpp
    unit/VerifierTest.cpp
    unit/SchemaWriterTest.cpp
    unit/AuthStrategyTest.cpp
    unit/CryptoTest.cpp
    unit/HookTypingsEmitterTest.cpp
    unit/ImportFromOpenApiTest.cpp
    unit/JsonExtractionTest.cpp
    unit/HttpLlmClientTest.cpp
    unit/TopologyTest.cpp
    unit/DependencyResolverTest.cpp
    unit/CodecsTest.cpp
    unit/CookiesTest.cpp
    unit/MultipartBuilderTest.cpp
    unit/QuickJsHookRunnerTest.cpp
    unit/SchemaParserMultiFileTest.cpp
    unit/HeaderMaskingTest.cpp
    unit/SqliteHistoryStoreTest.cpp
    unit/SchemaValidationTest.cpp
)
target_include_directories(chainapi-engine-unit-tests
    PRIVATE ${CMAKE_SOURCE_DIR}/engine/src
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
    integration/HooksTest.cpp
    integration/PollingTest.cpp
    integration/RunOptionsTest.cpp
    integration/MultipartTest.cpp
    integration/SessionRefreshTest.cpp
    integration/ExtractionSourcesTest.cpp
    integration/CookieJarTest.cpp
    integration/TransportConfigTest.cpp
)
target_link_libraries(chainapi-engine-integration-tests
    PRIVATE
        chainapi::engine
        CURL::libcurl
        nlohmann_json::nlohmann_json
        GTest::gtest
        GTest::gtest_main
)
target_include_directories(chainapi-engine-integration-tests
    PRIVATE ${CMAKE_SOURCE_DIR}/engine/src
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
