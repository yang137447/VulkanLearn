add_executable(editor_viewport_tests
    editorViewportGeometryTests.cpp
    editorViewportGridTests.cpp
    editorViewportAxisIndicatorTests.cpp
    "${PROJECT_SOURCE_DIR}/source/ui/viewport/editorViewportGeometry.cpp"
    "${PROJECT_SOURCE_DIR}/source/render/editor/editorViewportGridGeometry.cpp")
target_include_directories(editor_viewport_tests PRIVATE "${PROJECT_SOURCE_DIR}/source")
target_compile_features(editor_viewport_tests PRIVATE cxx_std_17)
# 几何生成单元只依赖 Eigen 与 vulkan.hpp 的结构体定义，不链接渲染后端；
# 缓冲所有权（editorViewportGrid.cpp）需要真实 Vulkan 设备，因此不进入本目标。
# 角落坐标轴指示器的投影数学只依赖 Eigen，与 RmlUi 无关，因此可以在这里做模块测试。
#
# Json 是必需的：本目标读 uiRenderSnapshot.h / sceneNode.h，而它们经 cameraExposure.h
# 拉入 nlohmann。此前这里只写 Eigen 与 Vulkan 是**对的**，是 master 的物理光照与曝光
# 改动让快照头多了这一层依赖 —— 合并后必须跟着补，否则本目标无法编译。
target_link_libraries(editor_viewport_tests PRIVATE Eigen3 Vulkan::Vulkan Json)
vulkanlearn_add_gtest(editor_viewport_tests)
