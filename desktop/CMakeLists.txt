qt_standard_project_setup(REQUIRES 6.8)

qt_add_executable(chainapi-desktop WIN32 MACOSX_BUNDLE
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
    src/views/Formatting.cpp
    src/views/Formatting.h
    src/views/MainWindow.cpp
    src/views/MainWindow.h
    src/views/ProjectExplorerWidget.cpp
    src/views/ProjectExplorerWidget.h
    src/views/RequestEditorPanel.cpp
    src/views/RequestEditorPanel.h
    src/views/ResponseViewerPanel.cpp
    src/views/ResponseViewerPanel.h
    src/views/TimelinePanel.cpp
    src/views/TimelinePanel.h
)

target_link_libraries(chainapi-desktop
    PRIVATE
        chainapi::engine
        Qt6::Core
        Qt6::Concurrent
        Qt6::Gui
        Qt6::Widgets
)

target_include_directories(chainapi-desktop
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

set_target_properties(chainapi-desktop PROPERTIES
    OUTPUT_NAME "ChainAPI"
    AUTOMOC ON
    AUTORCC ON
    AUTOUIC ON
)

chainapi_set_warnings(chainapi-desktop)
chainapi_apply_sanitizers(chainapi-desktop)

# No boundary guard here — desktop is the one place Qt UI libraries are
# allowed (PRD §8.6 / Project Layout §2.5). The engine boundary is enforced
# on the engine and CLI targets, not here.

if(CHAINAPI_BUILD_TESTS)
    add_subdirectory(tests)
endif()
