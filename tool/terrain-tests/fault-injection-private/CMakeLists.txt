cmake_minimum_required(VERSION 3.20)
project(terrain_fault_injection_cpu LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(VL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
add_library(terrain_fault_controller STATIC
    "${VL_ROOT}/source/terrain/terrainFaultInjection.cpp"
    "${VL_ROOT}/source/engine/testing/terrainRvtFaultTests.cpp")
target_sources(terrain_fault_controller PRIVATE "${VL_ROOT}/source/core/runtimeEvidence.cpp")
target_include_directories(terrain_fault_controller PUBLIC
    "${VL_ROOT}/source" "${VL_ROOT}/extern/Json/include")
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
add_subdirectory("${VL_ROOT}/build/_deps/googletest-src" googletest EXCLUDE_FROM_ALL)
add_executable(terrain_fault_injection_tests
    "${VL_ROOT}/tool/terrain-tests/terrainFaultInjectionTests.cpp")
target_link_libraries(terrain_fault_injection_tests PRIVATE
    terrain_fault_controller GTest::gtest_main)
enable_testing()
add_test(NAME terrain_fault_injection_cpu COMMAND terrain_fault_injection_tests)
add_subdirectory("${VL_ROOT}/extern/BLAKE3" blake3)
add_executable(terrain_fault_slot_tests
    "${VL_ROOT}/tool/terrain-tests/terrainFaultSlotControlTests.cpp"
    "${VL_ROOT}/source/terrain/terrainCompositedResidency.cpp"
    "${VL_ROOT}/source/terrain/terrainCompositedVT.cpp"
    "${VL_ROOT}/source/terrain/terrainCompositedVTStore.cpp"
    "${VL_ROOT}/source/terrain/terrainCompositedProduction.cpp"
    "${VL_ROOT}/source/terrain/terrainCompositedAdjacency.cpp"
    "${VL_ROOT}/source/terrain/terrainRvtBudget.cpp"
    "${VL_ROOT}/source/shader/build/contentHash.cpp"
    "${VL_ROOT}/source/core/runtimeFileTrace.cpp"
    "${VL_ROOT}/source/core/tracingIfstream.cpp"
    "${VL_ROOT}/source/shader/build/atomicFile.cpp")
target_link_libraries(terrain_fault_slot_tests PRIVATE terrain_fault_controller GTest::gtest BLAKE3::blake3)
target_compile_options(terrain_fault_slot_tests PRIVATE -O0 -ffunction-sections -fdata-sections)
target_link_options(terrain_fault_slot_tests PRIVATE -Wl,--gc-sections)
add_test(NAME terrain_fault_slot_cpu COMMAND terrain_fault_slot_tests)
