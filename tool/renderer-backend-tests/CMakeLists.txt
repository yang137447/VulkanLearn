add_executable(vl_renderer_backend_tests
    object_resource_publication_test.cpp
    swapchain_frame_result_test.cpp
    scene_viewport_output_validation_test.cpp
    physical_lighting_test.cpp
    auto_exposure_test.cpp
    gpu_pass_timings_test.cpp
    ${PROJECT_SOURCE_DIR}/source/render/rendergraph/renderGraphCompiler.cpp
    ${PROJECT_SOURCE_DIR}/source/render/rendergraph/sceneViewportOutputValidation.cpp
)
target_include_directories(vl_renderer_backend_tests PRIVATE ${CMAKE_SOURCE_DIR}/source)
target_link_libraries(vl_renderer_backend_tests PRIVATE Vulkan::Vulkan Json Eigen3)
target_compile_definitions(vl_renderer_backend_tests PRIVATE
    VL_TEST_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
vulkanlearn_add_gtest(vl_renderer_backend_tests)
