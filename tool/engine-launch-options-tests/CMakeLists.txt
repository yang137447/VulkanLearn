add_executable(engine_launch_options_tests
    launch_options_test.cpp
    "${PROJECT_SOURCE_DIR}/source/engine/launchOptions.cpp")
target_include_directories(engine_launch_options_tests PRIVATE "${PROJECT_SOURCE_DIR}/source")
target_link_libraries(engine_launch_options_tests PRIVATE
    Vulkan::Vulkan Vulkan::shaderc_combined Eigen3 Json RmlUi::Core Spirv_Reflect)
vulkanlearn_add_gtest(engine_launch_options_tests)
