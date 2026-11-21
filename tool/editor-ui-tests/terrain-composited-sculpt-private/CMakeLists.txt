cmake_minimum_required(VERSION 3.20)
project(terrain_composited_sculpt_private LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
get_filename_component(VL_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(VL_GTEST_SOURCE "" CACHE PATH "Read-only GoogleTest source directory")
set(VL_OZZ_SOURCE "" CACHE PATH "Read-only ozz source directory")
if(NOT EXISTS "${VL_GTEST_SOURCE}/CMakeLists.txt")
    message(FATAL_ERROR "Set VL_GTEST_SOURCE to an existing GoogleTest checkout")
endif()
find_package(Threads REQUIRED)
add_subdirectory("${VL_GTEST_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/googletest" EXCLUDE_FROM_ALL)
add_executable(terrain_composited_sculpt_tests
    ../terrainCompositedSculptApplyTests.cpp
    "${VL_ROOT}/source/editor/terrain/terrainCompositedSculptApply.cpp"
    "${VL_ROOT}/source/editor/terrain/terrainCompositedSculptCommit.cpp"
    "${VL_ROOT}/source/editor/terrain/terrainEditSession.cpp"
    "${VL_ROOT}/source/terrain/terrainHeightmapEditor.cpp"
    "${VL_ROOT}/source/terrain/terrainHeightmap.cpp"
    "${VL_ROOT}/source/terrain/terrainMeshBuilder.cpp"
    "${VL_ROOT}/source/resource/image/hostImage.cpp"
    "${VL_ROOT}/source/world/world.cpp")
target_include_directories(terrain_composited_sculpt_tests PRIVATE
    "${VL_ROOT}/source"
    "${VL_ROOT}/extern/Eigen3"
    "${VL_ROOT}/extern/Json/include"
    "${VL_ROOT}/extern/VulkanSDK/include"
    "${VL_OZZ_SOURCE}/include")
target_link_libraries(terrain_composited_sculpt_tests PRIVATE GTest::gtest_main Threads::Threads)
if(MINGW OR CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(terrain_composited_sculpt_tests PRIVATE -O1 -flto -ffunction-sections -fdata-sections)
    target_link_options(terrain_composited_sculpt_tests PRIVATE -flto -Wl,--gc-sections)
else()
    message(FATAL_ERROR "This private CPU slice currently requires GNU-style section GC")
endif()
enable_testing()
add_test(NAME terrain_composited_sculpt_tests COMMAND terrain_composited_sculpt_tests)
