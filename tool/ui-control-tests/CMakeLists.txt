add_executable(vl_ui_control_tests ui_control_test.cpp)
target_include_directories(vl_ui_control_tests PRIVATE ${CMAKE_SOURCE_DIR}/source)
target_compile_definitions(vl_ui_control_tests PRIVATE
    VL_RUNTIME_CONTROL_DOCUMENT="${CMAKE_SOURCE_DIR}/ui/runtime-control.rml")
target_link_libraries(vl_ui_control_tests PRIVATE RmlUi::Core)
vulkanlearn_add_gtest(vl_ui_control_tests)
