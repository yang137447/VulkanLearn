file(GLOB sources "*.h" "*.cpp")

add_executable(main ${sources})
# file(
#   DOWNLOAD
#   https://github.com/cpm-cmake/CPM.cmake/releases/download/v0.40.8/CPM.cmake
#   ${CMAKE_CURRENT_BINARY_DIR}/cmake/CPM.cmake
#   EXPECTED_HASH SHA256=78ba32abdf798bc616bab7c73aac32a17bbd7b06ad9e26a6add69de8f3ae4791
# )
# include(${CMAKE_CURRENT_BINARY_DIR}/cmake/CPM.cmake)
# #GLM
# CPMAddPackage("gh:g-truc/glm#2d4c4b4")
target_link_libraries(main 
PRIVATE
    Vulkan
    SDL3::SDL3
    Shaderc
    Eigen3
    #glm::glm
    stb_image
    assimp
    )