file(GLOB sources "*.h" "*.cpp")

add_executable(main ${sources})

target_link_libraries(main 
PRIVATE
    Vulkan::Vulkan
    Vulkan::shaderc_combined
    SDL3::SDL3
    Eigen3
    stb_image
    assimp
    Json
    Spirv_Reflect
    )