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
# GLOBAL on the find-module packages (CURL, SQLite3) so their imported
# targets are visible in every subdirectory. Without GLOBAL, an imported
# target created by a find module is only visible in this directory and its
# subdirectories — and the nested scopes introduced by FetchContent in
# third_party/ (QtKeychain) can leave SQLite3::SQLite3 / CURL::libcurl
# unresolved when engine/ links against them (observed on Windows).
find_package(Qt6 6.8 REQUIRED COMPONENTS Core)
find_package(yaml-cpp CONFIG REQUIRED)
find_package(CURL REQUIRED GLOBAL)
find_package(SQLite3 REQUIRED GLOBAL)
find_package(nlohmann_json CONFIG REQUIRED)
# Qt6Keychain — optional OS-keychain backend for SecretStore, exposed as
# `Qt6Keychain::Qt6Keychain`. Kept QUIET/optional on purpose: the vcpkg
# `qtkeychain` port pulls qtbase from source, which blows past CI's job
# time budget (see vcpkg.json $qt-comment). Instead, provide it out-of-band
# alongside the aqtinstall Qt — point CMAKE_PREFIX_PATH at a QtKeychain
# build, or set -DQt6Keychain_DIR=<path>. When found, the engine compiles
# the real KeychainSecretStore (CHAINAPI_HAS_QTKEYCHAIN=1); when absent it
# falls back to the no-op store and {{secret.X}} references stay unresolved.
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
