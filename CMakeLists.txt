cmake_minimum_required(VERSION 3.20)

project(vulkan_learn LANGUAGES C CXX)

include(CTest)

option(VULKANLEARN_BUILD_TESTS
    "Build VulkanLearn-owned unit and integration test executables."
    ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

find_package(Threads REQUIRED)

option(VULKANLEARN_ENABLE_DEVELOPER_UI
    "Build the Dear ImGui developer UI layer."
    ON)

add_subdirectory(extern)
add_subdirectory(source)

if(VULKANLEARN_BUILD_TESTS)
    enable_testing()

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
                WORKING_DIRECTORY "${VL_GTEST_WORKING_DIRECTORY}")
        else()
            gtest_discover_tests(${target})
        endif()
    endfunction()

    add_subdirectory(tool/shader-build-core-tests)
    add_subdirectory(tool/shader-build-integration-tests)
    add_subdirectory(tool/thin-translucent-tests)
    add_subdirectory(tool/subsurface-tests)
    add_subdirectory(tool/hair-tests)
    add_subdirectory(tool/cloth-tests)
    add_subdirectory(tool/material-schema-tests)
    add_subdirectory(tool/texture-asset-tests)
    add_subdirectory(tool/material-instance-editor-tests)

    add_subdirectory(tool/hair-lut-generator)
    add_subdirectory(tool/eye-tests)
    add_subdirectory(tool/eye-authoring-adapter)
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
