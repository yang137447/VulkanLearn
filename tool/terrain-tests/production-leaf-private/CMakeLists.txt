cmake_minimum_required(VERSION 3.20)
project(terrain_production_leaf_tests LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
get_filename_component(VL_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(VL_GTEST_SOURCE "" CACHE PATH "Read-only GoogleTest checkout")
if(NOT EXISTS "${VL_GTEST_SOURCE}/CMakeLists.txt")
    message(FATAL_ERROR "Set VL_GTEST_SOURCE to an existing GoogleTest checkout")
endif()
find_package(Threads REQUIRED)
add_subdirectory("${VL_GTEST_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/googletest" EXCLUDE_FROM_ALL)
add_subdirectory("${VL_ROOT}/extern/BLAKE3" "${CMAKE_CURRENT_BINARY_DIR}/blake3")
add_executable(terrain_production_leaf_tests
    ../terrainCompositedProductionLeafTests.cpp
    ../terrainCompositedProductionGpuTests.cpp
    "${VL_ROOT}/source/terrain/terrainCompositedProduction.cpp"
    "${VL_ROOT}/source/terrain/terrainCompositedAdjacency.cpp"
    "${VL_ROOT}/source/terrain/terrainRvtBudget.cpp"
    "${VL_ROOT}/source/terrain/terrainCompositedVT.cpp"
    "${VL_ROOT}/source/resource/image/hostImage.cpp"
    "${VL_ROOT}/source/shader/build/contentHash.cpp"
    "${VL_ROOT}/source/shader/build/atomicFile.cpp"
    "${VL_ROOT}/source/core/runtimeFileTrace.cpp"
    "${VL_ROOT}/source/core/tracingIfstream.cpp")
target_include_directories(terrain_production_leaf_tests PRIVATE
    "${VL_ROOT}/source"
    "${VL_ROOT}/extern/Json/include"
    "${VL_ROOT}/extern/VulkanSDK/include")
target_link_libraries(terrain_production_leaf_tests PRIVATE
    GTest::gtest_main Threads::Threads BLAKE3::blake3)
target_compile_definitions(terrain_production_leaf_tests PRIVATE VL_FILE_IO_TEST_HOOKS)
add_library(terrain_production_gpu_compile OBJECT
    "${VL_ROOT}/source/render/resource/terrainCompositedProductionGpu.cpp")
target_include_directories(terrain_production_gpu_compile PRIVATE
    "${VL_ROOT}/source"
    "${VL_ROOT}/extern/Json/include"
    "${VL_ROOT}/extern/VulkanSDK/include"
    "${VL_ROOT}/extern/Eigen3")
if(MINGW OR CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(terrain_production_leaf_tests PRIVATE -O1 -ffunction-sections -fdata-sections)
    target_link_options(terrain_production_leaf_tests PRIVATE -Wl,--gc-sections)
endif()
enable_testing()
include(GoogleTest)
gtest_discover_tests(terrain_production_leaf_tests DISCOVERY_TIMEOUT 30)
