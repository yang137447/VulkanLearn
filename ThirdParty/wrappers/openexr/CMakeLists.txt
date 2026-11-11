cmake_minimum_required(VERSION 3.24)

# Configure OpenEXR as a subproject so its top-level-only targets (website/src,
# docs, tests, clang-format) are skipped. ExternalProject still owns the
# install prefix and build tree.
project(vulkanlearn_openexr_wrapper LANGUAGES C CXX)

if(NOT VL_OPENEXR_SOURCE_DIR OR
    NOT EXISTS "${VL_OPENEXR_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "VL_OPENEXR_SOURCE_DIR must point at the OpenEXR source tree")
endif()

set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPENEXR_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(OPENEXR_INSTALL_TOOLS OFF CACHE BOOL "" FORCE)
set(OPENEXR_INSTALL_DEVELOPER_TOOLS OFF CACHE BOOL "" FORCE)
set(OPENEXR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(OPENEXR_BUILD_PYTHON OFF CACHE BOOL "" FORCE)
set(OPENEXR_FORCE_INTERNAL_IMATH ON CACHE BOOL "" FORCE)
set(OPENEXR_FORCE_INTERNAL_DEFLATE ON CACHE BOOL "" FORCE)
set(OPENEXR_FORCE_INTERNAL_OPENJPH ON CACHE BOOL "" FORCE)
set(BUILD_WEBSITE OFF CACHE BOOL "" FORCE)
set(OPENEXR_INSTALL_DOCS OFF CACHE BOOL "" FORCE)

add_subdirectory("${VL_OPENEXR_SOURCE_DIR}" openexr)
