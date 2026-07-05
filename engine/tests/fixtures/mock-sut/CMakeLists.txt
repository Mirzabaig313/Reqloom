# mock-sut — minimal HTTP server for engine integration tests.
#
# Built only when REQLOOM_BUILD_TESTS is ON. Not part of the shipped binary.

find_package(httplib CONFIG REQUIRED)

add_executable(reqloom-mock-sut
    main.cpp
)
target_link_libraries(reqloom-mock-sut
    PRIVATE
        httplib::httplib
        nlohmann_json::nlohmann_json
)
reqloom_set_warnings(reqloom-mock-sut)
reqloom_apply_sanitizers(reqloom-mock-sut)

# Mock SUT is test infrastructure — must not pull in Qt UI either.
reqloom_forbid_dependencies(reqloom-mock-sut
    Qt6::Widgets
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickWidgets
)
