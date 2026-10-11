add_executable(editor_viewport_tests
    editorViewportGeometryTests.cpp
    editorViewportGridTests.cpp
    "${PROJECT_SOURCE_DIR}/source/ui/viewport/editorViewportGeometry.cpp"
    "${PROJECT_SOURCE_DIR}/source/render/editor/editorViewportGridGeometry.cpp")
target_include_directories(editor_viewport_tests PRIVATE "${PROJECT_SOURCE_DIR}/source")
target_compile_features(editor_viewport_tests PRIVATE cxx_std_17)
# 几何生成单元只依赖 Eigen 与 vulkan.hpp 的结构体定义，不链接渲染后端；
# 缓冲所有权（editorViewportGrid.cpp）需要真实 Vulkan 设备，因此不进入本目标。
target_link_libraries(editor_viewport_tests PRIVATE Eigen3 Vulkan::Vulkan)
vulkanlearn_add_gtest(editor_viewport_tests)
