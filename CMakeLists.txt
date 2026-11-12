cmake_minimum_required(VERSION 3.24)

project(vulkan_learn LANGUAGES C CXX)

include(CTest)
include(cmake/VulkanLearnCompilerCache.cmake)
include(cmake/VulkanLearnDepsCache.cmake)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # Keep -g paths stable across worktrees; CCACHE_BASEDIR makes them relative.
    add_compile_options(
        "-fdebug-prefix-map=${CMAKE_SOURCE_DIR}=."
        "-fmacro-prefix-map=${CMAKE_SOURCE_DIR}=.")
endif()

option(VULKANLEARN_BUILD_TESTS
    "Build VulkanLearn-owned unit and integration test executables."
    ON)

option(VULKANLEARN_BUILD_ANIMATION_TOOLS
    "Build animation asset import and preflight tools."
    OFF)

option(VULKANLEARN_ENABLE_CSHARP_BLUEPRINTS "Build and host C# gameplay Blueprints (requires .NET 11 SDK)." OFF)

option(VULKANLEARN_ENABLE_TRACY "Enable Tracy profiler markers and client connection." OFF)
option(VULKANLEARN_ENABLE_NVTX "Enable NVIDIA NVTX profiler markers." OFF)
option(VULKANLEARN_ENABLE_VULKAN_VALIDATION "Enable Vulkan validation layer and debug markers." ON)

option(VULKANLEARN_ENABLE_EDITOR
    "Build editor libraries and the editor executable."
    ON)

option(VULKANLEARN_BUILD_GAME
    "Build the runtime-only game executable."
    ON)

option(VULKANLEARN_BUILD_LEGACY_MAIN
    "Copy main_editor.exe to main.exe for existing scripts."
    ON)

set(VULKANLEARN_DEPS_MODE "source" CACHE STRING
    "Third-party dependencies: 'source' builds extern/ in-tree, 'prebuilt' consumes the machine-wide prefix.")
set_property(CACHE VULKANLEARN_DEPS_MODE PROPERTY STRINGS source prebuilt)
set(VULKANLEARN_DEPS_ROOT "" CACHE PATH
    "Machine-wide dependency root used by VULKANLEARN_DEPS_MODE=prebuilt.")
set(VULKANLEARN_DEPS_PREBUILT "SDL3;Assimp;OpenEXR;RmlUi;FreeType;Jolt" CACHE STRING
    "Heavy dependencies taken from the prebuilt prefix; the rest stay in-tree.")
set(VULKANLEARN_DEPS_PREFIX "" CACHE PATH
    "Resolved dependency prefix; normally computed from VULKANLEARN_DEPS_ROOT.")

if(NOT VULKANLEARN_DEPS_MODE MATCHES "^(source|prebuilt)$")
    message(FATAL_ERROR
        "VULKANLEARN_DEPS_MODE must be 'source' or 'prebuilt'; got '${VULKANLEARN_DEPS_MODE}'.")
endif()

if(VULKANLEARN_DEPS_MODE STREQUAL "prebuilt")
    set(VULKANLEARN_DEPS_SKIP "${VULKANLEARN_DEPS_PREBUILT}")
else()
    set(VULKANLEARN_DEPS_SKIP "")
endif()

if(NOT VULKANLEARN_ENABLE_EDITOR AND NOT VULKANLEARN_BUILD_GAME)
    message(FATAL_ERROR
        "VULKANLEARN_ENABLE_EDITOR and VULKANLEARN_BUILD_GAME cannot both be OFF; "
        "at least one executable flavor must be selected.")
endif()

set(VULKANLEARN_TERRAIN_EROSION_PYTHON "" CACHE FILEPATH
    "Optional Python interpreter with numpy for terrain_erosion_tests.py.")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

find_package(Threads REQUIRED)

if(VULKANLEARN_DEPS_MODE STREQUAL "prebuilt")
    include(cmake/VulkanLearnDeps.cmake)
    vl_deps_configure_prebuilt()
endif()

if(NOT "Jolt" IN_LIST VULKANLEARN_DEPS_SKIP)
    include(cmake/VulkanLearnJolt.cmake)
    vl_add_jolt_cpu()
endif()

add_subdirectory(extern)
add_subdirectory(source/physics)
add_subdirectory(script)
add_subdirectory(source/render/offline/core)
add_subdirectory(source/animation)
add_subdirectory(source/debug)
add_subdirectory(source/hair)
add_subdirectory(source)
if(TARGET vl_deps_stamp_check AND TARGET vl_runtime)
    add_dependencies(vl_runtime vl_deps_stamp_check)
endif()
if(VULKANLEARN_ENABLE_EDITOR)
    add_subdirectory(tool/asset-edit)
endif()
add_subdirectory(tool/game-export)
add_subdirectory(tool/hair)
if(VULKANLEARN_BUILD_ANIMATION_TOOLS)
    add_subdirectory(tool/animation)
endif()

if(VULKANLEARN_BUILD_TESTS)
    enable_testing()

    if(VULKANLEARN_TERRAIN_EROSION_PYTHON AND
        EXISTS "${VULKANLEARN_TERRAIN_EROSION_PYTHON}")
        add_test(
            NAME terrain_erosion_tests
            COMMAND "${VULKANLEARN_TERRAIN_EROSION_PYTHON}"
                "${CMAKE_SOURCE_DIR}/tool/blender-terrain-import/terrain_erosion_tests.py"
                -v)
    endif()

    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
        GIT_SHALLOW TRUE
    )
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    include(GoogleTest)

    function(vulkanlearn_add_gtest target)
        cmake_parse_arguments(VL_GTEST "" "WORKING_DIRECTORY" "" ${ARGN})
        target_link_libraries(${target} PRIVATE GTest::gtest_main)
        if(VL_GTEST_WORKING_DIRECTORY)
            gtest_discover_tests(${target}
                WORKING_DIRECTORY "${VL_GTEST_WORKING_DIRECTORY}"
                DISCOVERY_TIMEOUT 30)
        else()
            gtest_discover_tests(${target} DISCOVERY_TIMEOUT 30)
        endif()
    endfunction()

    add_subdirectory(tool/shader-build-core-tests)
    add_subdirectory(tool/shader-build-integration-tests)
    add_subdirectory(tool/material-schema-tests)
    add_subdirectory(tool/texture-asset-tests)
    if(VULKANLEARN_ENABLE_EDITOR)
        add_subdirectory(tool/terrain-tests)
        add_subdirectory(tool/material-instance-editor-tests)
        add_subdirectory(tool/editor-ui-tests)
        add_subdirectory(tool/editor-viewport-tests)
        add_subdirectory(tool/script-tests)
    endif()
    add_subdirectory(tool/offline-core-tests)
    add_subdirectory(tool/offline-integration-tests)
    add_subdirectory(tool/renderer-backend-tests)
    add_subdirectory(tool/ui-control-tests)
    add_subdirectory(tool/controller-tests)
    add_subdirectory(tool/debug-draw-tests)
    add_subdirectory(tool/hair-tests)
    add_subdirectory(tool/game-export-tests)
    add_subdirectory(tool/engine-launch-options-tests)
    add_subdirectory(tool/fracture-core-tests)
    add_subdirectory(tool/audio-runtime-tests)
    if(TARGET vl_physics_runtime)
        add_subdirectory(tool/physics-runtime-tests)
    endif()
    if(VULKANLEARN_BUILD_ANIMATION_TOOLS)
        add_subdirectory(tool/animation-tests)
    endif()
endif()

option(VULKANLEARN_BUILD_SPEEDTREE_WIND_VALIDATION
    "Build the optional SpeedTree SDK/VulkanLearn numeric wind validator."
    ON)
set(SPEEDTREE_MODELER_ROOT "C:/Software/SpeedTree_10.2.0_extracted/{app}"
    CACHE PATH "SpeedTree Modeler installation containing standard_shaders/SpeedTreeWind.h")

if(VULKANLEARN_BUILD_SPEEDTREE_WIND_VALIDATION)
    if(EXISTS "${SPEEDTREE_MODELER_ROOT}/standard_shaders/SpeedTreeWind.h")
        add_subdirectory(tool/speedtree-wind-validation)
    else()
        message(STATUS
            "SpeedTree wind validator disabled: SpeedTree Modeler root was not found at "
            "${SPEEDTREE_MODELER_ROOT}")
    endif()
endif()
