# set(SDL3_DIR "${CMAKE_CURRENT_LIST_DIR}")
# if(NOT TARGET SDL3)
#     if(WIN32)
#         add_library(SDL3 SHARED IMPORTED GLOBAL)
#         set_target_properties(SDL3
#             PROPERTIES
#                 INTERFACE_INCLUDE_DIRECTORIES "${SDL3_DIR}/include"
#                 IMPORTED_LOCATION "${SDL3_DIR}/bin/SDL3.dll"
#                 IMPORTED_IMPLIB "${SDL3_DIR}/lib/libSDL3.lib"
#                 INTERFACE_LINK_LIBRARIES "-L${SDL3_DIR}/lib -lSDL3 -mwindows")
#     else()
#         find_package(SDL3 REQUIRED)
#     endif()
# endif()

#download CPM.cmake
file(
  DOWNLOAD
  https://github.com/cpm-cmake/CPM.cmake/releases/download/v0.40.8/CPM.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/cmake/CPM.cmake
  EXPECTED_HASH SHA256=78ba32abdf798bc616bab7c73aac32a17bbd7b06ad9e26a6add69de8f3ae4791
)
include(${CMAKE_CURRENT_BINARY_DIR}/cmake/CPM.cmake)

#SDL3
CPMAddPackage("gh:libsdl-org/SDL#f2bcfe3")