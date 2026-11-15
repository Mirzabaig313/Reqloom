# QML widget tests: load real widget files from qml/widgets into a QQuickWindow
# and drive them with key events. `import Reqloom` resolves to the stub module
# in ui/qml, because the real module is embedded in the app executable.
find_package(Qt6 REQUIRED COMPONENTS Quick QuickControls2 Test)
add_executable(reqloom-desktop-qml-tests
    ${CMAKE_CURRENT_LIST_DIR}/../TestMain.cpp
    ${CMAKE_CURRENT_LIST_DIR}/QmlWidgetsTest.cpp
)
target_compile_definitions(reqloom-desktop-qml-tests
    PRIVATE
        REQLOOM_QML_TEST_STUBS_DIR="${CMAKE_CURRENT_LIST_DIR}/qml"
        REQLOOM_QML_WIDGETS_DIR="${CMAKE_CURRENT_LIST_DIR}/../../qml/widgets"
)
target_link_libraries(reqloom-desktop-qml-tests
    PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Test
        GTest::gtest
)
reqloom_set_warnings(reqloom-desktop-qml-tests)
reqloom_apply_sanitizers(reqloom-desktop-qml-tests)
reqloom_isolate_test_config(reqloom-desktop-qml-tests)

gtest_discover_tests(reqloom-desktop-qml-tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES
        LABELS "desktop_ui"
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    DISCOVERY_TIMEOUT 60
)
