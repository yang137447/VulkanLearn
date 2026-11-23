cmake_minimum_required(VERSION 3.20)
project(terrain_weight_overlay_cpu LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
get_filename_component(VL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
add_library(overlay_cpu
    "${VL_ROOT}/source/terrain/terrainCompositedWeightOverlay.cpp"
    "${VL_ROOT}/source/terrain/terrainLayerPaint.cpp"
    "${VL_ROOT}/source/terrain/terrainLayerRegistry.cpp"
    "${VL_ROOT}/source/terrain/terrainHeightmap.cpp"
    "${VL_ROOT}/source/terrain/terrainPaintFile.cpp"
    "${VL_ROOT}/source/terrain/terrainPaintManifest.cpp"
    "${VL_ROOT}/source/terrain/terrainPaintSha256.cpp"
    "${VL_ROOT}/source/shader/build/atomicFile.cpp"
    "${VL_ROOT}/source/core/runtimeFileTrace.cpp"
    "${VL_ROOT}/source/core/tracingIfstream.cpp"
    "${VL_ROOT}/source/resource/image/hostImage.cpp")
target_include_directories(overlay_cpu PUBLIC
    "${VL_ROOT}/source" "${VL_ROOT}/extern/Eigen3"
    "${VL_ROOT}/extern/Json/include" "${VL_ROOT}/extern/STB")
add_executable(weight_overlay_audit audit.cpp)
target_link_libraries(weight_overlay_audit PRIVATE overlay_cpu)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
add_subdirectory("${VL_ROOT}/build/_deps/googletest-src" googletest EXCLUDE_FROM_ALL)
add_executable(weight_overlay_tests
    "${VL_ROOT}/tool/terrain-tests/terrainCompositedWeightOverlayTests.cpp")
target_link_libraries(weight_overlay_tests PRIVATE overlay_cpu GTest::gtest_main)
