add_executable(vl_controller_tests
    controller_test.cpp
    ${CMAKE_SOURCE_DIR}/source/controller.cpp
    ${CMAKE_SOURCE_DIR}/source/sceneNode.cpp)
target_include_directories(vl_controller_tests PRIVATE ${CMAKE_SOURCE_DIR}/source)
target_link_libraries(vl_controller_tests PRIVATE Eigen3 Vulkan::Vulkan Json)
vulkanlearn_add_gtest(vl_controller_tests)
