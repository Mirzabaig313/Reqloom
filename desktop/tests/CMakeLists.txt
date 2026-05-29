find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

# View-level tests. ResponseViewerPanel is self-contained (Qt-only, no engine
# dependency), so we compile it straight into the test binary rather than
# exposing the desktop sources as a library. AUTOMOC handles its Q_OBJECT.
add_executable(chainapi-desktop-ui-tests
    ui/ResponseViewerPanelTest.cpp
    ui/EnvironmentSettingsTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/views/ResponseViewerPanel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/views/ResponseViewerPanel.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/application/EnvironmentSettings.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/application/EnvironmentSettings.h
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
    PROPERTIES
        LABELS "desktop_ui"
        # Headless: no display server on CI runners.
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    DISCOVERY_TIMEOUT 60
)
