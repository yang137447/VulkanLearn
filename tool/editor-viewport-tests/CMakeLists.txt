add_executable(editor_viewport_tests
    editorViewportGeometryTests.cpp
    "${PROJECT_SOURCE_DIR}/source/ui/viewport/editorViewportGeometry.cpp")
target_include_directories(editor_viewport_tests PRIVATE "${PROJECT_SOURCE_DIR}/source")
target_compile_features(editor_viewport_tests PRIVATE cxx_std_17)
vulkanlearn_add_gtest(editor_viewport_tests)
