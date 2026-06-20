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

# ─── Scintilla (permissive — the desktop hook-script CodeEditor engine) ───────
# Scintilla's license (Neil Hodgson, MIT-style) permits use in this Apache-2.0
# tree. We deliberately do NOT use QScintilla (Riverbank, GPL/commercial). We
# fetch the official pinned tarball (hash-verified) and compile Scintilla's core
# plus its own permissive Qt platform layer (qt/ScintillaEditBase) against the
# project's Qt6. No Lexilla: syntax styling is driven from our own code via
# Scintilla's container-lexer notifications, so we don't compile ~100 lexers.
if(REQLOOM_BUILD_DESKTOP AND NOT TARGET scintilla)
    include(FetchContent)
    # Source the vendored in-tree tarball so the build needs no network
    # access (CI runners with restricted egress cannot reach scintilla.org).
    # A single local path is a valid URL entry; the file is hash-verified.
    FetchContent_Declare(
        scintilla
        URL "${CMAKE_CURRENT_SOURCE_DIR}/vendor/scintilla563.tgz"
        URL_HASH SHA256=f64339c504960c5a95510e6c3306ab5e95f23abaf8aed82897e57bff78e74616
    )
    FetchContent_MakeAvailable(scintilla)

    # ExternalProject strips a single top-level dir, but be robust to either
    # layout (SOURCE_DIR or SOURCE_DIR/scintilla).
    set(_sc "${scintilla_SOURCE_DIR}")
    if(NOT EXISTS "${_sc}/src/Editor.cxx")
        set(_sc "${scintilla_SOURCE_DIR}/scintilla")
    endif()

    add_library(scintilla STATIC
        ${_sc}/src/AutoComplete.cxx
        ${_sc}/src/CallTip.cxx
        ${_sc}/src/CaseConvert.cxx
        ${_sc}/src/CaseFolder.cxx
        ${_sc}/src/CellBuffer.cxx
        ${_sc}/src/ChangeHistory.cxx
        ${_sc}/src/CharClassify.cxx
        ${_sc}/src/CharacterCategoryMap.cxx
        ${_sc}/src/CharacterType.cxx
        ${_sc}/src/ContractionState.cxx
        ${_sc}/src/DBCS.cxx
        ${_sc}/src/Decoration.cxx
        ${_sc}/src/Document.cxx
        ${_sc}/src/EditModel.cxx
        ${_sc}/src/EditView.cxx
        ${_sc}/src/Editor.cxx
        ${_sc}/src/Geometry.cxx
        ${_sc}/src/Indicator.cxx
        ${_sc}/src/KeyMap.cxx
        ${_sc}/src/LineMarker.cxx
        ${_sc}/src/MarginView.cxx
        ${_sc}/src/PerLine.cxx
        ${_sc}/src/PositionCache.cxx
        ${_sc}/src/RESearch.cxx
        ${_sc}/src/RunStyles.cxx
        ${_sc}/src/ScintillaBase.cxx
        ${_sc}/src/Selection.cxx
        ${_sc}/src/Style.cxx
        ${_sc}/src/UndoHistory.cxx
        ${_sc}/src/UniConversion.cxx
        ${_sc}/src/UniqueString.cxx
        ${_sc}/src/ViewStyle.cxx
        ${_sc}/src/XPM.cxx
        ${_sc}/qt/ScintillaEditBase/PlatQt.cpp
        ${_sc}/qt/ScintillaEditBase/ScintillaQt.cpp
        ${_sc}/qt/ScintillaEditBase/ScintillaEditBase.cpp
    )
    set_target_properties(scintilla PROPERTIES AUTOMOC ON)
    target_include_directories(scintilla
        SYSTEM PUBLIC
            ${_sc}/include
            ${_sc}/qt/ScintillaEditBase
            ${_sc}/src
    )
    target_link_libraries(scintilla PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Core5Compat)
    target_compile_features(scintilla PRIVATE cxx_std_17)
    # Vendored third-party: silence its warnings (we don't lint upstream code).
    if(MSVC)
        target_compile_options(scintilla PRIVATE /w)
    else()
        target_compile_options(scintilla PRIVATE -w)
    endif()
endif()
