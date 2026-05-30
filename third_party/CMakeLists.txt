# third_party/CMakeLists.txt
#
# Vendored / fetched dependencies that don't come from vcpkg.
#
# QuickJS will be vendored here in Phase 1 (Project Layout Q3): it is
# small, single-file, slow-moving, and avoids dependency-pinning issues.

# ─── QtKeychain ───────────────────────────────────────────────────────────────
# QtKeychain (frankosterfeld/qtkeychain) is the OS-keychain backend for the
# engine's SecretStore. It is NOT part of Qt and is not shipped by aqtinstall,
# and the vcpkg `qtkeychain` port drags qtbase from source (45-90 min cold
# builds — see vcpkg.json $qt-comment). So we build it from source here via
# FetchContent, linking against the aqtinstall-provided Qt6. It's a tiny
# library (~20-30s to build) and its own find_package(Qt6) resolves to the
# same Qt the rest of the project uses via CMAKE_PREFIX_PATH.
#
# Skipped if the consumer already provided Qt6Keychain via find_package
# (top-level CMakeLists runs `find_package(Qt6Keychain CONFIG QUIET)` first).
if(NOT TARGET Qt6Keychain::Qt6Keychain)
    include(FetchContent)

    # Build options for QtKeychain — set before FetchContent_MakeAvailable.
    set(BUILD_WITH_QT6 ON CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)   # static — no runtime lib to ship
    set(BUILD_TEST_APPLICATION OFF CACHE BOOL "" FORCE)
    set(BUILD_TRANSLATIONS OFF CACHE BOOL "" FORCE)  # avoids needing Qt's lrelease tool

    FetchContent_Declare(
        qtkeychain
        GIT_REPOSITORY https://github.com/frankosterfeld/qtkeychain.git
        GIT_TAG v0.14.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(qtkeychain)

    if(TARGET qt6keychain)
        # QtKeychain's in-tree target only declares an INSTALL_INTERFACE
        # include dir, and its public header sits at the source root as
        # `keychain.h`. The engine includes it as <qt6keychain/keychain.h>
        # (the installed layout). Build a small shim include tree that
        # mirrors that layout so the include resolves without installing.
        set(_qtkc_shim "${CMAKE_CURRENT_BINARY_DIR}/qtkeychain-include")
        file(MAKE_DIRECTORY "${_qtkc_shim}/qt6keychain")
        # Public API header + the generated export header.
        configure_file(
            "${qtkeychain_SOURCE_DIR}/keychain.h"
            "${_qtkc_shim}/qt6keychain/keychain.h"
            COPYONLY)
        # generate_export_header writes qkeychain_export.h into the binary
        # dir; keychain.h includes it. Mirror it into the shim too.
        file(GENERATE
            OUTPUT "${_qtkc_shim}/qt6keychain/qkeychain_export.h"
            INPUT  "${qtkeychain_BINARY_DIR}/qkeychain_export.h")

        target_include_directories(qt6keychain PUBLIC
            $<BUILD_INTERFACE:${_qtkc_shim}>)

        # Namespaced alias to match the find_package() consumption form the
        # engine checks for: `if(TARGET Qt6Keychain::Qt6Keychain)`.
        if(NOT TARGET Qt6Keychain::Qt6Keychain)
            add_library(Qt6Keychain::Qt6Keychain ALIAS qt6keychain)
        endif()
    endif()
endif()
