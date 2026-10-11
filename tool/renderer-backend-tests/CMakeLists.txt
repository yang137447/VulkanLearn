add_executable(vl_renderer_backend_tests
    swapchain_frame_result_test.cpp
    scene_viewport_output_validation_test.cpp
    ${PROJECT_SOURCE_DIR}/source/render/rendergraph/renderGraphCompiler.cpp
    ${PROJECT_SOURCE_DIR}/source/render/rendergraph/sceneViewportOutputValidation.cpp
)
target_include_directories(vl_renderer_backend_tests PRIVATE ${CMAKE_SOURCE_DIR}/source)
target_link_libraries(vl_renderer_backend_tests PRIVATE Vulkan::Vulkan Json)
vulkanlearn_add_gtest(vl_renderer_backend_tests)
