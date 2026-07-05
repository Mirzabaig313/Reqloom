# engine/CMakeLists.txt
#
# Builds libreqloom-engine: a pure-C++ library, no Qt UI dependency.
# This boundary is enforced via reqloom_forbid_dependencies(...) at the
# bottom of this file.

# ─── Layered OBJECT libraries ────────────────────────────────────────────────
# Each layer compiles independently and is linked into the public umbrella
# target `reqloom-engine`. Layer separation lets us evolve infrastructure
# implementations (libcurl ↔ QNetworkAccessManager, sqlite ↔ etc.) without
# disturbing domain code.

# Domain layer: pure business logic, no I/O, no third-party deps beyond
# yaml-cpp/json for value types if absolutely required.
add_library(reqloom-engine-domain OBJECT
    src/domain/DependencyResolver.cpp
    src/domain/Topology.cpp
    src/domain/VariableResolver.cpp
    src/domain/RunContext.cpp
    src/domain/ErrorCodes.cpp
    src/domain/Codecs.cpp
)
target_include_directories(reqloom-engine-domain
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
# Domain layer is currently std-only.
reqloom_set_warnings(reqloom-engine-domain)
reqloom_apply_sanitizers(reqloom-engine-domain)


# Infrastructure layer: I/O adapters. Concrete implementations behind the
# pure interfaces declared in `engine/include/reqloom/engine/*.h`.
add_library(reqloom-engine-infrastructure OBJECT
    src/infrastructure/http/CurlHttpClient.cpp
    src/infrastructure/storage/SqliteHistoryStore.cpp
    src/infrastructure/secrets/KeychainSecretStore.cpp
    src/infrastructure/schema/YamlSchemaParser.cpp
    src/infrastructure/schema/YamlSchemaWriter.cpp
    src/infrastructure/hooks/QuickJsHookRunner.cpp
    src/infrastructure/extraction/JsonPathEvaluator.cpp
    src/infrastructure/util/Crypto.cpp
    src/infrastructure/typings/StaticHookTypingsEmitter.cpp
    src/infrastructure/llm/HttpLlmClient.cpp
)
target_include_directories(reqloom-engine-infrastructure
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
find_package(OpenSSL REQUIRED)
find_package(qjs CONFIG REQUIRED)
target_link_libraries(reqloom-engine-infrastructure
    PUBLIC  reqloom-engine-domain
    PRIVATE
        Qt6::Core
        CURL::libcurl
        yaml-cpp::yaml-cpp
        SQLite3::SQLite3
        nlohmann_json::nlohmann_json
        OpenSSL::Crypto
        qjs
)
if(TARGET Qt6Keychain::Qt6Keychain)
    target_link_libraries(reqloom-engine-infrastructure
        PRIVATE Qt6Keychain::Qt6Keychain)
    target_compile_definitions(reqloom-engine-infrastructure
        PRIVATE REQLOOM_HAS_QTKEYCHAIN=1)
endif()
reqloom_set_warnings(reqloom-engine-infrastructure)
reqloom_apply_sanitizers(reqloom-engine-infrastructure)


# Application layer: use cases. Orchestrates domain + infrastructure.
add_library(reqloom-engine-application OBJECT
    src/application/RunOperationUseCase.cpp
    src/application/ImportFromOpenApi.cpp
    src/application/ImportFromPostman.cpp
    src/application/ImportFromInsomnia.cpp
    src/application/ImportFromThunderClient.cpp
    src/application/ImportFromHoppscotch.cpp
    src/application/ImportFromHttpFile.cpp
    src/application/ImportFromBruno.cpp
    src/application/ImportFromHttpie.cpp
    src/application/ImportFromApidog.cpp
    src/application/ExecutionEngine.cpp
    src/application/Factories.cpp
    src/application/PredicateEvaluator.cpp
    src/application/Verifier.cpp
    src/application/AuthStrategy.cpp
    src/application/Cookies.cpp
    src/application/JsonExtraction.cpp
    src/application/MultipartBuilder.cpp
    src/application/RequestSigners.cpp
    src/application/HeaderMasking.cpp
)
target_include_directories(reqloom-engine-application
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(reqloom-engine-application
    PUBLIC  reqloom-engine-domain
    PRIVATE
        reqloom-engine-infrastructure
        nlohmann_json::nlohmann_json
)
reqloom_set_warnings(reqloom-engine-application)
reqloom_apply_sanitizers(reqloom-engine-application)


# ─── Public umbrella library ─────────────────────────────────────────────────
add_library(reqloom-engine STATIC)
target_link_libraries(reqloom-engine
    PUBLIC
        reqloom-engine-domain
        reqloom-engine-application
    PRIVATE
        reqloom-engine-infrastructure
)
target_include_directories(reqloom-engine
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
add_library(reqloom::engine ALIAS reqloom-engine)


# ─── Boundary enforcement ────────────────────────────────────────────────────
# The engine MUST NOT depend on Qt UI or the QScintilla code editor.
set(_reqloom_engine_forbidden
    Qt6::Widgets
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickWidgets
    QScintilla::QScintilla
)
foreach(_t
    reqloom-engine
    reqloom-engine-domain
    reqloom-engine-application
    reqloom-engine-infrastructure
)
    reqloom_forbid_dependencies(${_t} ${_reqloom_engine_forbidden})
endforeach()


# ─── Tests ───────────────────────────────────────────────────────────────────
if(REQLOOM_BUILD_TESTS)
    add_subdirectory(tests)
endif()
