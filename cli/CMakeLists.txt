# cli/CMakeLists.txt
#
# Builds the chainapi binary. Links libchainapi-engine + Qt6::Core only.
# Renderers are split into a small OBJECT library so the test target can
# exercise them without re-listing every source.

# ─── Renderers (text / JSON / JUnit) — reusable in tests ──────────────────────
add_library(chainapi-cli-renderers OBJECT
    src/output/TextRenderer.cpp
    src/output/JsonRenderer.cpp
    src/output/JUnitRenderer.cpp
)
target_include_directories(chainapi-cli-renderers
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(chainapi-cli-renderers
    PUBLIC chainapi::engine
)
chainapi_set_warnings(chainapi-cli-renderers)
chainapi_apply_sanitizers(chainapi-cli-renderers)


# ─── chainapi binary ─────────────────────────────────────────────────────────
add_executable(chainapi-cli
    src/main.cpp
    src/commands/RunCommand.cpp
    src/commands/LintCommand.cpp
    src/commands/ImportCommand.cpp
)

target_link_libraries(chainapi-cli
    PRIVATE
        chainapi-cli-renderers
        chainapi::engine
        Qt6::Core
)

set_target_properties(chainapi-cli PROPERTIES OUTPUT_NAME "chainapi")

chainapi_set_warnings(chainapi-cli)
chainapi_apply_sanitizers(chainapi-cli)

# CLI must not pull in Qt UI libraries even transitively.
chainapi_forbid_dependencies(chainapi-cli
    Qt6::Widgets
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickWidgets
)


# ─── Tests ───────────────────────────────────────────────────────────────────
if(CHAINAPI_BUILD_TESTS)
    add_subdirectory(tests)
endif()
