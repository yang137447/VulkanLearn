add_executable(vl_offline_adapter_tests
    offline_camera_motion_policy_test.cpp
    offline_mesh_adapter_test.cpp
    offline_scene_adapter_test.cpp
    offline_display_data_test.cpp
    offline_mode_feedback_test.cpp
    scene_object_type_support_test.cpp
    ${CMAKE_SOURCE_DIR}/source/render/offline/offlineMeshAdapter.cpp
    ${CMAKE_SOURCE_DIR}/source/render/offline/offlineSceneAdapter.cpp
    ${CMAKE_SOURCE_DIR}/source/render/offline/offlineColorImage.cpp
    ${CMAKE_SOURCE_DIR}/source/resource/image/textureIO.cpp
    ${CMAKE_SOURCE_DIR}/source/resource/image/ddsTexture.cpp
    ${CMAKE_SOURCE_DIR}/source/resource/image/ktx2Texture.cpp
    ${CMAKE_SOURCE_DIR}/source/resource/image/hostImage.cpp)
target_include_directories(vl_offline_adapter_tests PRIVATE ${CMAKE_SOURCE_DIR}/source)
target_link_libraries(vl_offline_adapter_tests PRIVATE
    vl_offline_core Eigen3 Vulkan::Vulkan Json stb_image OpenEXR::OpenEXR)
vulkanlearn_add_gtest(vl_offline_adapter_tests)

add_executable(vl_offline_coordinator_tests
    offline_render_coordinator_test.cpp
    offline_particle_test.cpp
    ${CMAKE_SOURCE_DIR}/source/render/offline/offlineRenderCoordinator.cpp
    ${CMAKE_SOURCE_DIR}/source/render/offline/offlineParticleSimulation.cpp
    ${CMAKE_SOURCE_DIR}/source/render/offline/offlineParticleCompositor.cpp
    ${CMAKE_SOURCE_DIR}/source/render/offline/offlineMeshAdapter.cpp
    ${CMAKE_SOURCE_DIR}/source/render/offline/offlineSceneAdapter.cpp
    ${CMAKE_SOURCE_DIR}/source/render/offline/offlineColorImage.cpp
    ${CMAKE_SOURCE_DIR}/source/particles/particleAssetJsonHelpers.cpp
    ${CMAKE_SOURCE_DIR}/source/particles/particleAssetCollisionAssets.cpp
    ${CMAKE_SOURCE_DIR}/source/particles/particleAssetParseEmitter.cpp
    ${CMAKE_SOURCE_DIR}/source/particles/particleAssetValidateEmitter.cpp
    ${CMAKE_SOURCE_DIR}/source/particles/particleAssetModuleSchema.cpp
    ${CMAKE_SOURCE_DIR}/source/particles/particleAssetLoader.cpp
    ${CMAKE_SOURCE_DIR}/source/resource/image/textureIO.cpp
    ${CMAKE_SOURCE_DIR}/source/resource/image/ddsTexture.cpp
    ${CMAKE_SOURCE_DIR}/source/resource/image/ktx2Texture.cpp
    ${CMAKE_SOURCE_DIR}/source/resource/image/hostImage.cpp
    ${CMAKE_SOURCE_DIR}/source/shader/build/contentHash.cpp
    ${CMAKE_SOURCE_DIR}/source/core/runtimeResult.cpp)
target_include_directories(vl_offline_coordinator_tests PRIVATE ${CMAKE_SOURCE_DIR}/source)
target_compile_definitions(vl_offline_coordinator_tests PRIVATE
    VL_PARTICLE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(vl_offline_coordinator_tests PRIVATE
    vl_offline_core Eigen3 Vulkan::Vulkan Json stb_image OpenEXR::OpenEXR BLAKE3::blake3)
vulkanlearn_add_gtest(vl_offline_coordinator_tests)
