find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

# Mock SUT — used as a CTest fixture for integration tests.
add_subdirectory(fixtures/mock-sut)

# ─── Unit tests ──────────────────────────────────────────────────────────────
# Smoke test: confirms libreqloom-engine builds, links, and the public
# error-code mapping is stable.
# VariableResolverTest: pinned to the domain layer (PRD §5.7), needs the
# private `engine/src/` include path to reach `domain/VariableResolver.h`.
add_executable(reqloom-engine-unit-tests
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
    unit/SecretEnvTagTest.cpp
    unit/ValidateProjectTest.cpp
)
target_include_directories(reqloom-engine-unit-tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/src
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(reqloom-engine-unit-tests
    PRIVATE
        reqloom::engine
        GTest::gtest
        GTest::gtest_main
)
reqloom_set_warnings(reqloom-engine-unit-tests)
reqloom_apply_sanitizers(reqloom-engine-unit-tests)

gtest_discover_tests(reqloom-engine-unit-tests
    PROPERTIES LABELS "engine_unit"
    # Sanitizer-instrumented binaries linking QtCore/OpenSSL/curl can take
    # several seconds just to start up and reach --gtest_list_tests on a
    # loaded CI runner. The default 5s discovery timeout is too tight there.
    DISCOVERY_TIMEOUT 60
)

# ─── Integration tests ───────────────────────────────────────────────────────
# Exercise the engine end-to-end against the mock SUT.
add_executable(reqloom-engine-integration-tests
    integration/MockSutHarness.h
    integration/MockSutHarness.cpp
    integration/HappyPathTest.cpp
    integration/HooksTest.cpp
    integration/PollingTest.cpp
    integration/ForEachTest.cpp
    integration/RunOptionsTest.cpp
    integration/MultipartTest.cpp
    integration/SessionRefreshTest.cpp
    integration/ExtractionSourcesTest.cpp
    integration/CookieJarTest.cpp
    integration/TransportConfigTest.cpp
    integration/SecretLoadingTest.cpp
)
target_link_libraries(reqloom-engine-integration-tests
    PRIVATE
        reqloom::engine
        CURL::libcurl
        nlohmann_json::nlohmann_json
        GTest::gtest
        GTest::gtest_main
)
target_include_directories(reqloom-engine-integration-tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/src
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_definitions(reqloom-engine-integration-tests
    PRIVATE
        REQLOOM_MOCK_SUT_PATH="$<TARGET_FILE:reqloom-mock-sut>"
        REQLOOM_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
        REQLOOM_SAMPLES_DIR="${CMAKE_SOURCE_DIR}/samples"
)
reqloom_set_warnings(reqloom-engine-integration-tests)
reqloom_apply_sanitizers(reqloom-engine-integration-tests)

gtest_discover_tests(reqloom-engine-integration-tests
    PROPERTIES LABELS "engine_integration"
    DISCOVERY_TIMEOUT 60
)

# All test binaries must respect the same engine-side boundary.
foreach(_t
    reqloom-engine-unit-tests
    reqloom-engine-integration-tests
)
    reqloom_forbid_dependencies(${_t}
        Qt6::Widgets
        Qt6::Gui
        Qt6::Quick
        Qt6::QuickWidgets
    )
endforeach()
