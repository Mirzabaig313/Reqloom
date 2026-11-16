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
    unit/OAuth2AuthCodeFlowTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/application/OAuth2AuthCodeFlow.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/application/OAuth2AuthCodeFlow.h
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
    unit/BodyTextTest.cpp
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/widgets/BodyText.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/widgets/BodyText.h
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
        # OAuth2AuthCodeFlowTest binds the loopback callback listener.
        Qt6::Network
        Qt6::Qml
        Qt6::Widgets
        nlohmann_json::nlohmann_json
        # SavedResponseStore's database.
        SQLite3::SQLite3
        GTest::gtest
)
set_target_properties(reqloom-desktop-ui-tests PROPERTIES
    AUTOMOC ON
)
reqloom_set_warnings(reqloom-desktop-ui-tests)
reqloom_apply_sanitizers(reqloom-desktop-ui-tests)
reqloom_isolate_test_config(reqloom-desktop-ui-tests)

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
    unit/ImportJobTest.cpp
    unit/ProjectModelTest.cpp
    unit/ProjectModelRenameTest.cpp
    unit/ProjectEditsTest.cpp
    unit/RequestOverrideTest.cpp
    unit/TabModelTest.cpp
    unit/TabEditsTest.cpp
    unit/EnvironmentEditsTest.cpp
    unit/WorkspaceModelTest.cpp
    unit/ProjectTreeModelTest.cpp
    unit/PathEvalTest.cpp
    unit/AuthStepListModelTest.cpp
    unit/TimelineModelTest.cpp
    unit/ExecutionPreviewTest.cpp
    unit/RunControllerContextTest.cpp
    unit/RunControllerLifecycleTest.cpp
    unit/RunTabRouterTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/application/RunController.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/application/RunController.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/application/ExecutionPreview.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/application/ExecutionPreview.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/views/Formatting.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/views/Formatting.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../qml/bridge/models/TimelineModel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../qml/bridge/models/TimelineModel.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../qml/bridge/appcontroller/RunTabRouter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../qml/bridge/appcontroller/RunTabRouter.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../qml/bridge/models/TabModel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../qml/bridge/models/TabModel.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../qml/bridge/models/TabEdits.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../qml/bridge/models/TabEdits.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../qml/bridge/models/EnvironmentEdits.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../qml/bridge/models/EnvironmentEdits.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/widgets/LatencyStats.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/widgets/LatencyStats.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/application/SecretManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/application/SecretManager.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/application/ImportJob.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/application/ImportJob.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/application/ProjectModel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/application/ProjectModel.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/application/ProjectEdits.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/application/ProjectModelEntities.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/application/ProjectEdits.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/application/WorkspaceModel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/application/WorkspaceModel.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../qml/bridge/models/ProjectTreeModel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../qml/bridge/models/ProjectTreeModel.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../qml/bridge/models/AuthStepListModel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../qml/bridge/models/AuthStepListModel.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../qml/bridge/models/EditableKeyValueModel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../qml/bridge/models/EditableKeyValueModel.h
    unit/ResponseBodyModelTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../qml/bridge/models/ResponseBodyModel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../qml/bridge/models/ResponseBodyModel.h
)
target_include_directories(reqloom-desktop-secret-tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../qml/bridge
        ${CMAKE_CURRENT_SOURCE_DIR}/../qml/bridge/models
)
# Lets ExecutionPreviewTest resolve the real sample project, so the preview is
# checked against a project a user could actually open rather than only against
# hand-built structs. Mirrors the engine integration tests' definition.
target_compile_definitions(reqloom-desktop-secret-tests
    PRIVATE
        REQLOOM_SAMPLES_DIR="${CMAKE_SOURCE_DIR}/samples"
)
target_link_libraries(reqloom-desktop-secret-tests
    PRIVATE
        reqloom::engine
        Qt6::Core
        Qt6::Qml
        # RunController runs the engine on a QtConcurrent worker.
        Qt6::Concurrent
        # ResponseBodyModel parses bodies with nlohmann so integers keep their digits.
        nlohmann_json::nlohmann_json
        GTest::gtest
)
set_target_properties(reqloom-desktop-secret-tests PROPERTIES
    AUTOMOC ON
)
reqloom_set_warnings(reqloom-desktop-secret-tests)
reqloom_apply_sanitizers(reqloom-desktop-secret-tests)
reqloom_isolate_test_config(reqloom-desktop-secret-tests)

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

include(${CMAKE_CURRENT_SOURCE_DIR}/ui/QmlTests.cmake)
