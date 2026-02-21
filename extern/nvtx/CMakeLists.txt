cmake_minimum_required(VERSION 3.10)

# Suppress warning about FetchContent_Populate
if(POLICY CMP0169)
  cmake_policy(SET CMP0169 OLD)
endif()

include(FetchContent)

# Fetch NVTX headers
FetchContent_Declare(
    nvtx
    GIT_REPOSITORY https://github.com/NVIDIA/NVTX.git
    GIT_TAG v3.4.0
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/nvtx
)

FetchContent_GetProperties(nvtx)
if(NOT nvtx_POPULATED)
    FetchContent_Populate(nvtx)
endif()

# Define NVTX as an interface library
if(NOT TARGET nvtx)
    add_library(nvtx INTERFACE)
    target_include_directories(nvtx INTERFACE "${nvtx_SOURCE_DIR}/c/include")
    
    # Create alias for consistent linking
    add_library(NVTX::nvtx ALIAS nvtx)
endif()
