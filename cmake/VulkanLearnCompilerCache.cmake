include_guard(GLOBAL)

option(VULKANLEARN_USE_CCACHE
    "Route C/C++ compilation through ccache when it is available." ON)

if(NOT VULKANLEARN_USE_CCACHE)
    return()
endif()

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(STATUS "ccache: skipped for ${CMAKE_CXX_COMPILER_ID}")
    return()
endif()

find_program(VL_CCACHE_EXECUTABLE NAMES ccache)
if(NOT VL_CCACHE_EXECUTABLE)
    message(FATAL_ERROR
        "VULKANLEARN_USE_CCACHE=ON but ccache was not found. "
        "Install ccache or configure with -DVULKANLEARN_USE_CCACHE=OFF.")
endif()

set(CMAKE_C_COMPILER_LAUNCHER "${VL_CCACHE_EXECUTABLE}" CACHE STRING "" FORCE)
set(CMAKE_CXX_COMPILER_LAUNCHER "${VL_CCACHE_EXECUTABLE}" CACHE STRING "" FORCE)
message(STATUS "ccache: ${VL_CCACHE_EXECUTABLE}")
