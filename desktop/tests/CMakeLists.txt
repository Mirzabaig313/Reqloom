find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

# Logic + reused-component tests. These compile only the survivable C++ logic
# (settings, Color/Theme, FuzzyMatch, LineDiff, SavedResponseStore) straight
# into the test binary. The Widgets-bound UI tests (StatusBadge, KeyValueEditor,

add_executable(reqloom-desktop-ui-tests
    TestMain.cpp
    ui/EnvironmentSettingsTest.cpp
    unit/ColorTest.cpp
    unit/ContrastTest.cpp
    unit/DesignTokensTest.cpp
    unit/LayoutSettingsTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../qml/theming/DesignTokens.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../qml/theming/DesignTokens.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../qml/bridge/controllers/ThemeController.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../qml/bridge/controllers/ThemeController.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../qml/bridge/controllers/LayoutSettingsController.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../qml/bridge/controllers/LayoutSettingsController.h
    unit/FuzzyMatchTest.cpp
    unit/LineDiffTest.cpp
    unit/GraphLayoutTest.cpp
    unit/ConstraintLayoutTest.cpp
    unit/LatencyStatsTest.cpp
    unit/SavedResponseStoreTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/application/SavedResponseStore.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/application/SavedResponseStore.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/application/EnvironmentSettings.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/application/EnvironmentSettings.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/application/LayoutSettings.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/application/LayoutSettings.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/theming/Color.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/theming/Color.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/theming/Theme.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/theming/Theme.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/widgets/FuzzyMatch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/widgets/FuzzyMatch.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/widgets/LineDiff.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/widgets/LineDiff.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/widgets/GraphLayout.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/widgets/GraphLayout.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/widgets/ConstraintLayout.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/widgets/ConstraintLayout.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/widgets/LatencyStats.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/widgets/LatencyStats.h
)
target_include_directories(reqloom-desktop-ui-tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_CURRENT_SOURCE_DIR}/../qml/bridge/controllers
        ${CMAKE_CURRENT_SOURCE_DIR}/../qml/theming
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_link_libraries(reqloom-desktop-ui-tests
    PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Widgets
        GTest::gtest
)
set_target_properties(reqloom-desktop-ui-tests PROPERTIES
    AUTOMOC ON
)
reqloom_set_warnings(reqloom-desktop-ui-tests)
reqloom_apply_sanitizers(reqloom-desktop-ui-tests)

gtest_discover_tests(reqloom-desktop-ui-tests
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
# because it links reqloom::engine.
add_executable(reqloom-desktop-secret-tests
    unit/SecretManagerTest.cpp
    unit/ProjectModelTest.cpp
    unit/WorkspaceModelTest.cpp
    unit/ProjectTreeModelTest.cpp
    unit/PathEvalTest.cpp
    unit/AuthStepListModelTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/application/SecretManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/application/SecretManager.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/application/ProjectModel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/application/ProjectModel.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/application/WorkspaceModel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/application/WorkspaceModel.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../qml/bridge/models/ProjectTreeModel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../qml/bridge/models/ProjectTreeModel.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../qml/bridge/models/AuthStepListModel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../qml/bridge/models/AuthStepListModel.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../qml/bridge/models/EditableKeyValueModel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../qml/bridge/models/EditableKeyValueModel.h
)
target_include_directories(reqloom-desktop-secret-tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../qml/bridge
        ${CMAKE_CURRENT_SOURCE_DIR}/../qml/bridge/models
)
target_link_libraries(reqloom-desktop-secret-tests
    PRIVATE
        reqloom::engine
        Qt6::Core
        Qt6::Qml
        GTest::gtest
)
set_target_properties(reqloom-desktop-secret-tests PROPERTIES
    AUTOMOC ON
)
reqloom_set_warnings(reqloom-desktop-secret-tests)
reqloom_apply_sanitizers(reqloom-desktop-secret-tests)

gtest_discover_tests(reqloom-desktop-secret-tests
    # PRE_TEST defers enumeration to ctest run time. POST_BUILD (the default)
    # executes this Qt-linked binary during the build; on macOS that can block
    # (keychain/TCC access on an ad-hoc-signed binary, or Qt platform init)
    # past DISCOVERY_TIMEOUT and fail the build. PRE_TEST runs discovery at
    # test time with the offscreen platform in effect. Mirrors the UI tests.
    DISCOVERY_MODE PRE_TEST
    PROPERTIES
        LABELS "desktop_unit"
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    DISCOVERY_TIMEOUT 60
)
