# engine/CMakeLists.txt
#
# Builds libchainapi-engine: a pure-C++ library, no Qt UI dependency.
# This boundary is enforced via chainapi_forbid_dependencies(...) at the
# bottom of this file.

# ─── Layered OBJECT libraries ────────────────────────────────────────────────
# Each layer compiles independently and is linked into the public umbrella
# target `chainapi-engine`. Layer separation lets us evolve infrastructure
# implementations (libcurl ↔ QNetworkAccessManager, sqlite ↔ etc.) without
# disturbing domain code.

# Domain layer: pure business logic, no I/O, no third-party deps beyond
# yaml-cpp/json for value types if absolutely required.
add_library(chainapi-engine-domain OBJECT
    src/domain/DependencyResolver.cpp
    src/domain/Topology.cpp
    src/domain/VariableResolver.cpp
    src/domain/RunContext.cpp
    src/domain/Operation.cpp
    src/domain/Actor.cpp
    src/domain/Resource.cpp
    src/domain/ErrorCodes.cpp
)
target_include_directories(chainapi-engine-domain
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
# Domain layer is currently std-only. AGENTS.md permits Qt6::Core (for
# QString) but we have not needed it; keep this layer Qt-free as long as
# possible to ease the Phase B engine extraction.
chainapi_set_warnings(chainapi-engine-domain)
chainapi_apply_sanitizers(chainapi-engine-domain)


# Infrastructure layer: I/O adapters. Concrete implementations behind the
# pure interfaces declared in `engine/include/chainapi/engine/*.h`.
add_library(chainapi-engine-infrastructure OBJECT
    src/infrastructure/http/CurlHttpClient.cpp
    src/infrastructure/storage/SqliteHistoryStore.cpp
    src/infrastructure/secrets/KeychainSecretStore.cpp
    src/infrastructure/schema/YamlSchemaParser.cpp
    src/infrastructure/hooks/QuickJsHookRunner.cpp
    src/infrastructure/extraction/JsonPathEvaluator.cpp
)
target_include_directories(chainapi-engine-infrastructure
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(chainapi-engine-infrastructure
    PUBLIC  chainapi-engine-domain
    PRIVATE
        Qt6::Core
        CURL::libcurl
        yaml-cpp::yaml-cpp
        SQLite3::SQLite3
        nlohmann_json::nlohmann_json
)
if(TARGET Qt6Keychain::Qt6Keychain)
    target_link_libraries(chainapi-engine-infrastructure
        PRIVATE Qt6Keychain::Qt6Keychain)
    target_compile_definitions(chainapi-engine-infrastructure
        PRIVATE CHAINAPI_HAS_QTKEYCHAIN=1)
endif()
chainapi_set_warnings(chainapi-engine-infrastructure)
chainapi_apply_sanitizers(chainapi-engine-infrastructure)


# Application layer: use cases. Orchestrates domain + infrastructure.
add_library(chainapi-engine-application OBJECT
    src/application/RunOperationUseCase.cpp
    src/application/ImportFromOpenApi.cpp
    src/application/ExecutionEngine.cpp
)
target_include_directories(chainapi-engine-application
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(chainapi-engine-application
    PUBLIC  chainapi-engine-domain
    PRIVATE
        chainapi-engine-infrastructure
        nlohmann_json::nlohmann_json
)
chainapi_set_warnings(chainapi-engine-application)
chainapi_apply_sanitizers(chainapi-engine-application)


# ─── Public umbrella library ─────────────────────────────────────────────────
add_library(chainapi-engine STATIC)
target_link_libraries(chainapi-engine
    PUBLIC
        chainapi-engine-domain
        chainapi-engine-application
    PRIVATE
        chainapi-engine-infrastructure
)
target_include_directories(chainapi-engine
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
add_library(chainapi::engine ALIAS chainapi-engine)


# ─── Boundary enforcement ────────────────────────────────────────────────────
# The engine MUST NOT depend on Qt UI or the QScintilla code editor.
set(_chainapi_engine_forbidden
    Qt6::Widgets
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickWidgets
    QScintilla::QScintilla
)
foreach(_t
    chainapi-engine
    chainapi-engine-domain
    chainapi-engine-application
    chainapi-engine-infrastructure
)
    chainapi_forbid_dependencies(${_t} ${_chainapi_engine_forbidden})
endforeach()


# ─── Tests ───────────────────────────────────────────────────────────────────
if(CHAINAPI_BUILD_TESTS)
    add_subdirectory(tests)
endif()
