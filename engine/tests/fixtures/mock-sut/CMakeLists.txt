# mock-sut — minimal HTTP server for engine integration tests.
#
# Built only when CHAINAPI_BUILD_TESTS is ON. Not part of the shipped binary.

find_package(httplib CONFIG REQUIRED)

add_executable(chainapi-mock-sut
    main.cpp
)
target_link_libraries(chainapi-mock-sut
    PRIVATE
        httplib::httplib
        nlohmann_json::nlohmann_json
)
chainapi_set_warnings(chainapi-mock-sut)
chainapi_apply_sanitizers(chainapi-mock-sut)

# Mock SUT is test infrastructure — must not pull in Qt UI either.
chainapi_forbid_dependencies(chainapi-mock-sut
    Qt6::Widgets
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickWidgets
)
