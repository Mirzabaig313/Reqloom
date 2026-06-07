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

# No boundary guard here — desktop is the one place Qt UI libraries are
# allowed (PRD §8.6 / Project Layout §2.5). The engine boundary is enforced
# on the engine and CLI targets, not here.

if(REQLOOM_BUILD_TESTS)
    add_subdirectory(tests)
endif()
