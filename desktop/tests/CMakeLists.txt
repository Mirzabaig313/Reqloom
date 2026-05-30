find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

# View-level tests. ResponseViewerPanel is self-contained (Qt-only, no engine
# dependency), so we compile it straight into the test binary rather than
# exposing the desktop sources as a library. AUTOMOC handles its Q_OBJECT.
add_executable(chainapi-desktop-ui-tests
    ui/ResponseViewerPanelTest.cpp
    ui/EnvironmentSettingsTest.cpp
    unit/ColorTest.cpp
    unit/StatusBadgeTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/views/ResponseViewerPanel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/views/ResponseViewerPanel.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/application/EnvironmentSettings.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/application/EnvironmentSettings.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/theming/Color.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/theming/Color.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/theming/Theme.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/theming/Theme.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/widgets/StatusBadge.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/widgets/StatusBadge.h
)
target_include_directories(chainapi-desktop-ui-tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_link_libraries(chainapi-desktop-ui-tests
    PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        GTest::gtest
)
set_target_properties(chainapi-desktop-ui-tests PROPERTIES
    AUTOMOC ON
)
chainapi_set_warnings(chainapi-desktop-ui-tests)
chainapi_apply_sanitizers(chainapi-desktop-ui-tests)

gtest_discover_tests(chainapi-desktop-ui-tests
    # PRE_TEST defers test enumeration to ctest run time. The default
    # POST_BUILD mode executes the binary during the build, before the
    # ENVIRONMENT property below applies, so Qt would try the xcb plugin,
    # find no display on CI, and abort. PRE_TEST runs discovery with the
    # offscreen platform in effect.
    DISCOVERY_MODE PRE_TEST
    PROPERTIES
        LABELS "desktop_ui"
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    DISCOVERY_TIMEOUT 60
)

# SecretManager test: needs the engine (SecretStore interface, project parse,
# secret enumeration) and ProjectModel. Uses an in-memory SecretStore, so no
# real OS keychain is touched. Separate target from the Qt-only UI tests
# because it links chainapi::engine.
add_executable(chainapi-desktop-secret-tests
    unit/SecretManagerTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/application/SecretManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/application/SecretManager.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/application/ProjectModel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/application/ProjectModel.h
)
target_include_directories(chainapi-desktop-secret-tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_link_libraries(chainapi-desktop-secret-tests
    PRIVATE
        chainapi::engine
        Qt6::Core
        GTest::gtest
)
set_target_properties(chainapi-desktop-secret-tests PROPERTIES
    AUTOMOC ON
)
chainapi_set_warnings(chainapi-desktop-secret-tests)
chainapi_apply_sanitizers(chainapi-desktop-secret-tests)

gtest_discover_tests(chainapi-desktop-secret-tests
    PROPERTIES
        LABELS "desktop_unit"
    DISCOVERY_TIMEOUT 60
)
