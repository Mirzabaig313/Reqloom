cmake_minimum_required(VERSION 4.0)

project(chainapi
    VERSION 0.1.0
    DESCRIPTION "Workflow-aware API testing tool with auto-resolved dependency chains"
    LANGUAGES CXX
)

# ─── Standards ────────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# On Windows, prevent <windows.h> (pulled in transitively by curl, the Qt
# platform layer, and others) from defining the function-like `min`/`max`
# macros that collide with std::min/std::max and numeric_limits::max().
# WIN32_LEAN_AND_MEAN trims rarely-needed Win32 sub-headers, cutting both
# compile time and the surface for further macro clashes.
if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# ─── Build options ────────────────────────────────────────────────────────────
option(CHAINAPI_BUILD_DESKTOP "Build the Qt desktop app"          ON)
option(CHAINAPI_BUILD_CLI     "Build the CLI binary"              ON)
option(CHAINAPI_BUILD_TESTS   "Build the test suite"              ON)
option(CHAINAPI_BUILD_IPC     "Build engine IPC server (Phase B)" OFF)
option(CHAINAPI_ENABLE_ASAN   "Enable AddressSanitizer in Debug"  OFF)
option(CHAINAPI_ENABLE_UBSAN  "Enable UBSan in Debug"             OFF)

# ─── Module path ──────────────────────────────────────────────────────────────
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(ChainApiCompilerWarnings)
include(ChainApiSanitizers)
include(ChainApiBoundaryGuards)

# ─── Dependencies (used by every target) ──────────────────────────────────────
find_package(Qt6 6.8 REQUIRED COMPONENTS Core)
find_package(yaml-cpp CONFIG REQUIRED)
find_package(CURL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# SQLite3 target normalization.
#
# CMake's built-in FindSQLite3 module creates `SQLite3::SQLite3` when it
# locates the library on disk. vcpkg's sqlite3 port instead ships a config
# package (`unofficial-sqlite3`) that exposes `unofficial::sqlite3::sqlite3`.
# Depending on platform / how vcpkg's toolchain steers find_package, the
# module-mode target may not be created (observed on the Windows CI runner:
# find_package succeeds but SQLite3::SQLite3 is absent at generate time).
# Normalize to a single name the engine links against: SQLite3::SQLite3.
find_package(SQLite3 QUIET)
if(NOT TARGET SQLite3::SQLite3)
    find_package(unofficial-sqlite3 CONFIG REQUIRED)
    if(TARGET unofficial::sqlite3::sqlite3)
        # Promote to GLOBAL so the alias is usable from any subdirectory.
        set_target_properties(unofficial::sqlite3::sqlite3
            PROPERTIES IMPORTED_GLOBAL TRUE)
        add_library(SQLite3::SQLite3 ALIAS unofficial::sqlite3::sqlite3)
    else()
        message(FATAL_ERROR
            "SQLite3 found but neither SQLite3::SQLite3 nor "
            "unofficial::sqlite3::sqlite3 target exists.")
    endif()
endif()
# Qt6Keychain — OS-keychain backend for the engine's SecretStore, exposed
# as `Qt6Keychain::Qt6Keychain`. Tried QUIET first so a system/Homebrew
# install (or a machine where vcpkg's port is acceptable) is used as-is.
# When NOT found here, third_party/CMakeLists.txt builds it from source via
# FetchContent against the already-present Qt6 — deliberately avoiding the
# vcpkg `qtkeychain` port, which drags qtbase from source and blows past
# CI's job time budget (see vcpkg.json $qt-comment). Either way the engine
# ends up with the real KeychainSecretStore (CHAINAPI_HAS_QTKEYCHAIN=1).
find_package(Qt6Keychain CONFIG QUIET)

# UI-only deps
if(CHAINAPI_BUILD_DESKTOP)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Gui Quick QuickWidgets Concurrent)
    # QScintilla is added later when the desktop module starts using it.
endif()

# ─── Subdirectories ───────────────────────────────────────────────────────────
# Note: enable_testing() must run BEFORE any add_subdirectory() that calls
# add_test() / gtest_discover_tests(); otherwise the tests are silently
# dropped from CTest's view. See the CMake docs for enable_testing().
if(CHAINAPI_BUILD_TESTS)
    enable_testing()
    include(CTest)
endif()

add_subdirectory(third_party)
add_subdirectory(engine)

if(CHAINAPI_BUILD_CLI)
    add_subdirectory(cli)
endif()

if(CHAINAPI_BUILD_DESKTOP)
    add_subdirectory(desktop)
endif()

if(CHAINAPI_BUILD_IPC)
    add_subdirectory(ipc)
endif()

# ─── Boundary enforcement runs after all targets are declared ────────────────
chainapi_enforce_boundary_rules()

# ─── Status summary ───────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "ChainAPI configuration:")
message(STATUS "  Version:           ${PROJECT_VERSION}")
message(STATUS "  Build type:        ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard:      ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build desktop:     ${CHAINAPI_BUILD_DESKTOP}")
message(STATUS "  Build CLI:         ${CHAINAPI_BUILD_CLI}")
message(STATUS "  Build tests:       ${CHAINAPI_BUILD_TESTS}")
message(STATUS "  Build IPC:         ${CHAINAPI_BUILD_IPC}")
message(STATUS "  ASan:              ${CHAINAPI_ENABLE_ASAN}")
message(STATUS "  UBSan:             ${CHAINAPI_ENABLE_UBSAN}")
message(STATUS "")
