cmake_minimum_required(VERSION 3.20)
project(terrain_producer_reload_tests LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
get_filename_component(VL_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(VL_GTEST_SOURCE "${VL_ROOT}/build/_deps/googletest-src" CACHE PATH "Existing GoogleTest source")
add_subdirectory("${VL_GTEST_SOURCE}" "${CMAKE_BINARY_DIR}/googletest")
add_subdirectory("${VL_ROOT}/extern/BLAKE3" "${CMAKE_BINARY_DIR}/blake3")
add_executable(terrain_producer_reload_tests
    ../terrainCompositedProducerSnapshotTests.cpp
    "${VL_ROOT}/source/render/resource/terrainCompositedProducerSnapshot.cpp"
    "${VL_ROOT}/source/shader/reload/computeShaderArtifact.cpp"
    "${VL_ROOT}/source/shader/shaderAbiSignature.cpp"
    "${VL_ROOT}/source/shader/build/contentHash.cpp")
target_include_directories(terrain_producer_reload_tests PRIVATE
    "${VL_ROOT}/source"
    "${VL_ROOT}/extern/VulkanSDK/include"
    "${VL_ROOT}/extern/Shaderc/include"
    "${VL_ROOT}/extern/Spirv-Reflect/include")
target_link_libraries(terrain_producer_reload_tests PRIVATE GTest::gtest_main BLAKE3::blake3)
enable_testing()
add_test(NAME terrain_producer_reload_tests COMMAND terrain_producer_reload_tests)
add_executable(terrain_reload_registration_tests
    ../terrainCompositedReloadRegistrationTests.cpp
    "${VL_ROOT}/source/shader/reload/shaderReloadCoordinator.cpp"
    "${VL_ROOT}/source/shader/reload/computePipelineReloadParticipant.cpp"
    "${VL_ROOT}/source/pipeline/pipelineFactory.cpp"
    "${VL_ROOT}/source/shader/shaderAbiSignature.cpp"
    "${VL_ROOT}/source/shader/build/contentHash.cpp")
target_compile_options(terrain_reload_registration_tests PRIVATE
    "@${VL_ROOT}/build/source/CMakeFiles/vl_runtime.dir/includes_CXX.rsp"
    -flto -ffunction-sections -fdata-sections)
set_source_files_properties(../terrainCompositedReloadRegistrationTests.cpp
    PROPERTIES COMPILE_OPTIONS "-fno-access-control")
target_link_options(terrain_reload_registration_tests PRIVATE -flto -Wl,--gc-sections)
target_link_libraries(terrain_reload_registration_tests PRIVATE GTest::gtest_main BLAKE3::blake3)
add_test(NAME terrain_reload_registration_tests COMMAND terrain_reload_registration_tests)
