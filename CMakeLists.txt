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

option(VULKANLEARN_BUILD_SPEEDTREE_WIND_VALIDATION
    "Build the optional SpeedTree SDK/VulkanLearn numeric wind validator."
    ON)
set(SPEEDTREE_MODELER_ROOT "C:/Software/SpeedTree_10.2.0_extracted/{app}"
    CACHE PATH "SpeedTree Modeler installation containing standard_shaders/SpeedTreeWind.h")

if(VULKANLEARN_BUILD_SPEEDTREE_WIND_VALIDATION)
    if(EXISTS "${SPEEDTREE_MODELER_ROOT}/standard_shaders/SpeedTreeWind.h")
        enable_testing()
        add_subdirectory(tool/speedtree-wind-validation)
    else()
        message(STATUS
            "SpeedTree wind validator disabled: SpeedTree Modeler root was not found at "
            "${SPEEDTREE_MODELER_ROOT}")
    endif()
endif()

if(VULKANLEARN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tool/shader-build-core-tests)
    add_subdirectory(tool/shader-build-integration-tests)
    add_subdirectory(tool/thin-translucent-tests)
    add_subdirectory(tool/subsurface-tests)
endif()
