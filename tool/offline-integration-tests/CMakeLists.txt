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
    ${CMAKE_SOURCE_DIR}/source/resource/image/hostImage.cpp)
target_include_directories(vl_offline_adapter_tests PRIVATE ${CMAKE_SOURCE_DIR}/source)
target_link_libraries(vl_offline_adapter_tests PRIVATE
    vl_offline_core Eigen3 Vulkan::Vulkan Json stb_image OpenEXR::OpenEXR)
vulkanlearn_add_gtest(vl_offline_adapter_tests)

add_executable(vl_offline_coordinator_tests
    offline_render_coordinator_test.cpp
    ${CMAKE_SOURCE_DIR}/source/render/offline/offlineRenderCoordinator.cpp
    ${CMAKE_SOURCE_DIR}/source/render/offline/offlineMeshAdapter.cpp
    ${CMAKE_SOURCE_DIR}/source/render/offline/offlineSceneAdapter.cpp
    ${CMAKE_SOURCE_DIR}/source/render/offline/offlineColorImage.cpp
    ${CMAKE_SOURCE_DIR}/source/resource/image/textureIO.cpp
    ${CMAKE_SOURCE_DIR}/source/resource/image/hostImage.cpp
    ${CMAKE_SOURCE_DIR}/source/shader/build/contentHash.cpp
    ${CMAKE_SOURCE_DIR}/source/core/runtimeResult.cpp)
target_include_directories(vl_offline_coordinator_tests PRIVATE ${CMAKE_SOURCE_DIR}/source)
target_link_libraries(vl_offline_coordinator_tests PRIVATE
    vl_offline_core Eigen3 Vulkan::Vulkan Json stb_image OpenEXR::OpenEXR BLAKE3::blake3)
vulkanlearn_add_gtest(vl_offline_coordinator_tests)
