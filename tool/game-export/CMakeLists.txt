if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.24)
    project(vl_game_export_cpu LANGUAGES C CXX)
    get_filename_component(PROJECT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    add_subdirectory("${PROJECT_SOURCE_DIR}/extern/Json" "${CMAKE_BINARY_DIR}/json")
    add_subdirectory("${PROJECT_SOURCE_DIR}/extern/BLAKE3" "${CMAKE_BINARY_DIR}/blake3")
    set(VL_GAME_EXPORT_GTEST_SOURCE "" CACHE PATH "Existing GoogleTest sources for CPU exporter tests.")
    if(NOT EXISTS "${VL_GAME_EXPORT_GTEST_SOURCE}/CMakeLists.txt")
        message(FATAL_ERROR "Set VL_GAME_EXPORT_GTEST_SOURCE to existing GoogleTest sources; no download is performed.")
    endif()
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    add_subdirectory("${VL_GAME_EXPORT_GTEST_SOURCE}" "${CMAKE_BINARY_DIR}/googletest")
    enable_testing()
    include(GoogleTest)
    function(vulkanlearn_add_gtest target)
        target_link_libraries(${target} PRIVATE GTest::gtest_main)
        gtest_discover_tests(${target} DISCOVERY_TIMEOUT 30)
    endfunction()
endif()

add_library(vl_game_export STATIC gameExporter.cpp nativeDependencies.cpp
    "${PROJECT_SOURCE_DIR}/source/shader/build/contentHash.cpp"
    "${PROJECT_SOURCE_DIR}/source/shader/build/atomicFile.cpp"
    "${PROJECT_SOURCE_DIR}/source/core/runtimeFileTrace.cpp"
    "${PROJECT_SOURCE_DIR}/source/core/tracingIfstream.cpp"
    "${PROJECT_SOURCE_DIR}/source/terrain/terrainCompositedProduction.cpp"
    "${PROJECT_SOURCE_DIR}/source/terrain/terrainCompositedAdjacency.cpp"
    "${PROJECT_SOURCE_DIR}/source/terrain/terrainCompositedVT.cpp"
    "${PROJECT_SOURCE_DIR}/source/terrain/terrainCompositedVTStore.cpp"
    "${PROJECT_SOURCE_DIR}/source/terrain/terrainPaintSha256.cpp"
    "${PROJECT_SOURCE_DIR}/source/material/loader/materialInstanceResolver.cpp"
    "${PROJECT_SOURCE_DIR}/source/material/validation/materialAssetValidator.cpp"
    "${PROJECT_SOURCE_DIR}/source/material/materialDescriptorSchema.cpp")
target_sources(vl_game_export PRIVATE
    "${PROJECT_SOURCE_DIR}/source/material/generator/materialParameterIncludeGenerator.cpp")
target_include_directories(vl_game_export PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    PRIVATE "${PROJECT_SOURCE_DIR}/source"
    "${PROJECT_SOURCE_DIR}/extern/Eigen3"
    "${PROJECT_SOURCE_DIR}/extern/VulkanSDK/include"
    "${PROJECT_SOURCE_DIR}/extern/Spirv-Reflect/include")
target_link_libraries(vl_game_export PUBLIC Json PRIVATE BLAKE3::blake3)

add_executable(vl_export_game main.cpp)
target_link_libraries(vl_export_game PRIVATE vl_game_export)
if(MINGW)
    get_filename_component(vl_mingw_runtime_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    target_compile_definitions(vl_export_game PRIVATE
        VL_MINGW_RUNTIME_DIR="${vl_mingw_runtime_dir}")
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_subdirectory("${PROJECT_SOURCE_DIR}/tool/game-export-tests" "${CMAKE_BINARY_DIR}/tests")
endif()
if(VULKANLEARN_ENABLE_CSHARP_BLUEPRINTS)
    get_filename_component(export_dotnet_root "${VULKANLEARN_DOTNET_EXECUTABLE}" DIRECTORY)
    target_compile_definitions(vl_export_game PRIVATE VL_EXPORT_CSHARP_ENABLED
        VL_EXPORT_DOTNET_ROOT="${export_dotnet_root}")
endif()
