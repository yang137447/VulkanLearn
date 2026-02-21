cmake_minimum_required(VERSION 3.10)

# Suppress warning about FetchContent_Populate
if(POLICY CMP0169)
  cmake_policy(SET CMP0169 OLD)
endif()

include(FetchContent)

# Fetch Tracy Profiler source code
FetchContent_Declare(
    tracy
    GIT_REPOSITORY https://github.com/wolfpld/tracy.git
    GIT_TAG v0.13.1
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tracy
)

FetchContent_GetProperties(tracy)
if(NOT tracy_POPULATED)
    FetchContent_Populate(tracy)
endif()

# Define Tracy Client library manually to avoid building the server/tools
if(NOT TARGET TracyClient)
    add_library(TracyClient OBJECT "${tracy_SOURCE_DIR}/public/TracyClient.cpp")
    target_include_directories(TracyClient PUBLIC "${tracy_SOURCE_DIR}/public")
    target_compile_definitions(TracyClient PUBLIC TRACY_ENABLE TRACY_NO_SYSTEM_TRACING)
    
    # Create alias for consistent linking
    add_library(Tracy::TracyClient ALIAS TracyClient)
endif()
