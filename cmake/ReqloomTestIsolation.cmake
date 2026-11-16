# ReqloomTestIsolation.cmake
# Reusable function: reqloom_isolate_test_config(<target>)
# Every test executable sets REQLOOM_CONFIG_DIR to a directory under the build
# tree before main(), so neither ctest nor a directly run test binary writes
# the user's real per-user config (e.g. the project trust file).

set(REQLOOM_TEST_CONFIG_DIR "${CMAKE_BINARY_DIR}/test-config")

function(reqloom_isolate_test_config target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "reqloom_isolate_test_config: '${target}' is not a target")
    endif()
    target_sources(${target} PRIVATE "${PROJECT_SOURCE_DIR}/engine/tests/support/IsolatedConfigDir.cpp")
    target_compile_definitions(${target}
        PRIVATE REQLOOM_TEST_CONFIG_DIR="${REQLOOM_TEST_CONFIG_DIR}")
endfunction()
