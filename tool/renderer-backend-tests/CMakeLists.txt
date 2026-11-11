add_executable(vl_renderer_backend_tests
    object_resource_publication_test.cpp
    swapchain_frame_result_test.cpp
    scene_viewport_output_validation_test.cpp
    physical_lighting_test.cpp
    auto_exposure_test.cpp
    buffer_to_image_regions_test.cpp
    frame_epoch_ring_test.cpp
    resource_retire_queue_test.cpp
    world_snapshot_delta_test.cpp
    resolved_draw_lifetime_test.cpp
    render_frame_budget_test.cpp
    dynamic_mesh_update_queue_test.cpp
    terrain_feedback_pass_test.cpp
    terrain_feedback_readback_test.cpp
    terrain_layer_atlas_gpu_test.cpp
    gpu_pass_timings_test.cpp
    performance_snapshot_test.cpp
    cpu_frame_profiler_test.cpp
    shadow_caster_cull_test.cpp
    skin_deform_test.cpp
    speedtree_wind_system_test.cpp
    ${PROJECT_SOURCE_DIR}/source/render/rendergraph/renderGraphCompiler.cpp
    ${PROJECT_SOURCE_DIR}/source/render/rendergraph/sceneViewportOutputValidation.cpp
    ${PROJECT_SOURCE_DIR}/source/render/shadow/shadowCasterCull.cpp
    ${PROJECT_SOURCE_DIR}/source/render/foliage/speedTreeWindSystem.cpp
    ${PROJECT_SOURCE_DIR}/source/terrain/terrainFeedback.cpp
    ${PROJECT_SOURCE_DIR}/source/render/resource/resourceRetireQueue.cpp
    ${PROJECT_SOURCE_DIR}/source/world/worldSnapshot.cpp
)
target_include_directories(vl_renderer_backend_tests PRIVATE ${CMAKE_SOURCE_DIR}/source)
target_link_libraries(vl_renderer_backend_tests PRIVATE Vulkan::Vulkan Json Eigen3)
target_compile_definitions(vl_renderer_backend_tests PRIVATE
    VL_TEST_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
vulkanlearn_add_gtest(vl_renderer_backend_tests)
