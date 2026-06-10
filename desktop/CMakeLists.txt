qt_standard_project_setup(REQUIRES 6.8)

qt_add_executable(reqloom-desktop WIN32 MACOSX_BUNDLE
    src/main.cpp
    src/application/App.cpp
    src/application/App.h
    src/application/Bootstrapper.cpp
    src/application/Bootstrapper.h
    src/application/ProjectModel.cpp
    src/application/ProjectModel.h
    src/application/RunController.cpp
    src/application/RunController.h
    src/application/EnvironmentSettings.cpp
    src/application/EnvironmentSettings.h
    src/application/SecretManager.cpp
    src/application/SecretManager.h
    src/application/LayoutSettings.cpp
    src/application/LayoutSettings.h
    src/application/SavedResponseStore.cpp
    src/application/SavedResponseStore.h
    src/theming/Color.cpp
    src/theming/Color.h
    src/theming/Theme.cpp
    src/theming/Theme.h
    src/theming/ThemeManager.cpp
    src/theming/ThemeManager.h
    src/widgets/StatusBadge.cpp
    src/widgets/StatusBadge.h
    src/widgets/PanelHeader.cpp
    src/widgets/PanelHeader.h
    src/widgets/EmptyState.cpp
    src/widgets/EmptyState.h
    src/widgets/FuzzyMatch.cpp
    src/widgets/FuzzyMatch.h
    src/widgets/CommandPalette.cpp
    src/widgets/CommandPalette.h
    src/widgets/Toast.cpp
    src/widgets/Toast.h
    src/widgets/LineDiff.cpp
    src/widgets/LineDiff.h
    src/widgets/KeyValueEditor.cpp
    src/widgets/KeyValueEditor.h
    src/widgets/DependencyListEditor.cpp
    src/widgets/DependencyListEditor.h
    src/widgets/ExtractionTableEditor.cpp
    src/widgets/ExtractionTableEditor.h
    src/widgets/ChainView.cpp
    src/widgets/ChainView.h
    src/widgets/MethodItemDelegate.cpp
    src/widgets/MethodItemDelegate.h
    src/views/Formatting.cpp
    src/views/Formatting.h
    src/views/MainWindow.cpp
    src/views/MainWindow.h
    src/views/NewEndpointDialog.cpp
    src/views/NewEndpointDialog.h
    src/views/NewModuleDialog.cpp
    src/views/NewModuleDialog.h
    src/views/ProjectExplorerWidget.cpp
    src/views/ProjectExplorerWidget.h
    src/views/RequestEditorPanel.cpp
    src/views/RequestEditorPanel.h
    src/views/ResponseViewerPanel.cpp
    src/views/ResponseViewerPanel.h
    src/views/SecretsDialog.cpp
    src/views/SecretsDialog.h
    src/views/TimelinePanel.cpp
    src/views/TimelinePanel.h
)

target_link_libraries(reqloom-desktop
    PRIVATE
        reqloom::engine
        Qt6::Core
        Qt6::Concurrent
        Qt6::Gui
        Qt6::Widgets
)

target_include_directories(reqloom-desktop
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

set_target_properties(reqloom-desktop PROPERTIES
    OUTPUT_NAME "Reqloom"
    AUTOMOC ON
    AUTORCC ON
    AUTOUIC ON
)

reqloom_set_warnings(reqloom-desktop)
reqloom_apply_sanitizers(reqloom-desktop)

# ─── QML app (ADR-007) ────────────────────────────────────────────────────────
# New QML/Qt Quick front-end, built alongside the Widgets app during migration.
# Logic (engine, ProjectModel) is shared; presentation is QML. When QML reaches
# parity the Widgets target above is removed and this becomes reqloom-desktop.
qt_add_executable(reqloom-qml
    qml/main.cpp
    qml/theming/DesignTokens.h
    qml/theming/DesignTokens.cpp
    qml/bridge/ThemeController.h
    qml/bridge/ThemeController.cpp
    qml/bridge/SecretsController.h
    qml/bridge/SecretsController.cpp
    qml/bridge/ResourceListModel.cpp
    qml/bridge/ResourceListModel.h
    qml/bridge/OperationListModel.cpp
    qml/bridge/OperationListModel.h
    qml/bridge/KeyValueModel.cpp
    qml/bridge/KeyValueModel.h
    qml/bridge/ProjectTreeModel.cpp
    qml/bridge/ProjectTreeModel.h
    qml/bridge/ProjectTreeFilterModel.cpp
    qml/bridge/ProjectTreeFilterModel.h
    qml/bridge/DependencyEditModel.cpp
    qml/bridge/DependencyEditModel.h
    qml/bridge/EditableKeyValueModel.cpp
    qml/bridge/EditableKeyValueModel.h
    qml/bridge/TimelineModel.cpp
    qml/bridge/TimelineModel.h
    qml/bridge/ExampleListModel.cpp
    qml/bridge/ExampleListModel.h
    qml/bridge/AppController.cpp
    qml/bridge/AppController.h
    src/application/ProjectModel.cpp
    src/application/ProjectModel.h
    src/application/Bootstrapper.cpp
    src/application/Bootstrapper.h
    src/application/RunController.cpp
    src/application/RunController.h
    src/application/SavedResponseStore.cpp
    src/application/SavedResponseStore.h
    src/application/EnvironmentSettings.cpp
    src/application/EnvironmentSettings.h
    src/application/SecretManager.cpp
    src/application/SecretManager.h
    src/views/Formatting.cpp
    src/views/Formatting.h
    src/widgets/FuzzyMatch.cpp
    src/widgets/FuzzyMatch.h
    src/widgets/LineDiff.cpp
    src/widgets/LineDiff.h
    src/theming/Color.cpp
    src/theming/Color.h
    src/theming/Theme.cpp
    src/theming/Theme.h
)

qt_add_qml_module(reqloom-qml
    URI Reqloom
    VERSION 1.0
    QML_FILES
        qml/views/Main.qml
        qml/views/ExplorerPanel.qml
        qml/views/RequestEditor.qml
        qml/views/ResponsePanel.qml
        qml/views/TimelinePanel.qml
        qml/views/NewModuleDialog.qml
        qml/views/NewEndpointDialog.qml
        qml/widgets/MethodBadge.qml
        qml/widgets/Card.qml
        qml/widgets/GlassComboBox.qml
        qml/widgets/GlassButton.qml
        qml/widgets/GlassMenu.qml
        qml/widgets/GlassMenuItem.qml
        qml/widgets/DialogButtons.qml
        qml/widgets/GlassTextField.qml
        qml/widgets/FieldLabel.qml
        qml/widgets/DialogHeader.qml
        qml/widgets/AppIcon.qml
        qml/widgets/PanelHeader.qml
        qml/widgets/LineTabBar.qml
        qml/widgets/KeyValueList.qml
        qml/widgets/KeyValueEditorView.qml
        qml/widgets/ChainView.qml
        qml/widgets/StatusBadge.qml
        qml/widgets/DependencyEditor.qml
        qml/widgets/ExtractionEditor.qml
        qml/views/SecretsDialog.qml
        qml/widgets/Toast.qml
        qml/widgets/EmptyState.qml
    RESOURCES
        qml/icons/chevron-left.svg
        qml/icons/chevron-right.svg
        qml/icons/chevron-up.svg
        qml/icons/chevron-down.svg
        qml/icons/columns.svg
        qml/icons/rows.svg
        qml/icons/x.svg
        qml/icons/search.svg
        qml/icons/sun.svg
        qml/icons/moon.svg
        qml/icons/monitor.svg
        qml/icons/plus.svg
        qml/icons/user.svg
        qml/icons/folder.svg
        qml/icons/folder-open.svg
        qml/icons/zap.svg
        qml/icons/arrow-down.svg
        qml/icons/paperclip.svg
)

target_link_libraries(reqloom-qml
    PRIVATE
        reqloom::engine
        Qt6::Core
        Qt6::Concurrent
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        # Widgets is linked so a non-replaceable Widgets-only control (e.g. a
        # QScintilla code editor) can be shown as a standalone dialog window.
        # QML-first; Widgets only as a fallback (ADR-007).
        Qt6::Widgets
)

target_include_directories(reqloom-qml
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/qml/bridge
        ${CMAKE_CURRENT_SOURCE_DIR}/qml/theming
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

set_target_properties(reqloom-qml PROPERTIES
    OUTPUT_NAME "ReqloomQml"
    MACOSX_BUNDLE ON
)

reqloom_set_warnings(reqloom-qml)
reqloom_apply_sanitizers(reqloom-qml)

# Point the runtime font loader at the repo's desktop/fonts during development
# so dropping Geist / Noto Sans JP .ttf files there registers them with no
# rebuild of resources. A shipped app loads from its bundle Resources/fonts.
target_compile_definitions(reqloom-qml
    PRIVATE REQLOOM_FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fonts")

# No boundary guard here — desktop is the one place Qt UI libraries are
# allowed (PRD §8.6 / Project Layout §2.5). The engine boundary is enforced
# on the engine and CLI targets, not here.

if(REQLOOM_BUILD_TESTS)
    add_subdirectory(tests)
endif()
