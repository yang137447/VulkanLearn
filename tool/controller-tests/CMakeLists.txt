add_executable(vl_controller_tests
    controller_test.cpp
    ${CMAKE_SOURCE_DIR}/source/controller.cpp
    ${CMAKE_SOURCE_DIR}/source/sceneNode.cpp
    # commonFunction.h 的路径/配置读取经 TracingIfstream/RuntimeFileTrace 记账，需要两者定义。
    ${CMAKE_SOURCE_DIR}/source/core/runtimeFileTrace.cpp
    ${CMAKE_SOURCE_DIR}/source/core/tracingIfstream.cpp)
target_include_directories(vl_controller_tests PRIVATE ${CMAKE_SOURCE_DIR}/source)
target_link_libraries(vl_controller_tests PRIVATE Eigen3 Vulkan::Vulkan Json)
vulkanlearn_add_gtest(vl_controller_tests)
