cmake_minimum_required(VERSION 3.20)
project(frame_abandon_validation LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(VL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
find_package(Vulkan REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
add_subdirectory("${VL_ROOT}/build/_deps/googletest-src" googletest EXCLUDE_FROM_ALL)
add_executable(frame_abandon_cpu_tests
    "${VL_ROOT}/tool/renderer-backend-tests/frameAbandonTests.cpp"
    "${VL_ROOT}/source/render/frame/frameSubmitFault.cpp"
    "${VL_ROOT}/source/core/runtimeEvidence.cpp"
    "${VL_ROOT}/source/terrain/terrainFaultInjection.cpp")
target_include_directories(frame_abandon_cpu_tests PRIVATE
    "${VL_ROOT}/source" "${VL_ROOT}/extern/Json/include")
target_link_libraries(frame_abandon_cpu_tests PRIVATE GTest::gtest_main Vulkan::Headers)
add_executable(framesubmitabort
    "${VL_ROOT}/tool/renderer-backend-tests/frameAbandonVulkanHarness.cpp"
    "${VL_ROOT}/source/render/backend/frameAbandonVulkan.cpp")
target_include_directories(framesubmitabort PRIVATE "${VL_ROOT}/source")
target_link_libraries(framesubmitabort PRIVATE Vulkan::Vulkan SDL3::SDL3)
add_custom_command(TARGET framesubmitabort POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:SDL3::SDL3-shared> $<TARGET_FILE_DIR:framesubmitabort>)
add_executable(framesubmitabort_short
    "${VL_ROOT}/tool/renderer-backend-tests/frameAbandonVulkanHarness.cpp"
    "${VL_ROOT}/source/render/backend/frameAbandonVulkan.cpp")
target_include_directories(framesubmitabort_short PRIVATE "${VL_ROOT}/source")
target_compile_definitions(framesubmitabort_short PRIVATE VL_FRAME_ABORT_SHORT=1)
target_link_libraries(framesubmitabort_short PRIVATE Vulkan::Vulkan SDL3::SDL3)
add_custom_command(TARGET framesubmitabort_short POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:SDL3::SDL3-shared> $<TARGET_FILE_DIR:framesubmitabort_short>)
enable_testing()
add_test(NAME frame_abandon_cpu COMMAND frame_abandon_cpu_tests)
add_executable(frame_identity_cpu_tests
    "${VL_ROOT}/tool/renderer-backend-tests/gpuFrameIdentityTests.cpp")
target_include_directories(frame_identity_cpu_tests PRIVATE "${VL_ROOT}/source" "${VL_ROOT}/extern/Json/include")
target_link_libraries(frame_identity_cpu_tests PRIVATE GTest::gtest_main)
add_test(NAME frame_identity_cpu COMMAND frame_identity_cpu_tests)
add_executable(frame_evidence_sink_tests
    "${VL_ROOT}/tool/renderer-backend-tests/frameEvidenceSinkTests.cpp"
    "${VL_ROOT}/source/core/runtimeEvidence.cpp"
    "${VL_ROOT}/source/render/frame/frameSubmitFault.cpp"
    "${VL_ROOT}/source/terrain/terrainFaultInjection.cpp"
    "${VL_ROOT}/source/engine/testing/terrainRvtFaultTests.cpp")
target_include_directories(frame_evidence_sink_tests PRIVATE "${VL_ROOT}/source" "${VL_ROOT}/extern/Json/include")
target_link_libraries(frame_evidence_sink_tests PRIVATE GTest::gtest_main)
add_test(NAME frame_evidence_sink_cpu COMMAND frame_evidence_sink_tests)
