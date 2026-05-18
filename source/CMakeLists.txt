file(GLOB_RECURSE sources CONFIGURE_DEPENDS "*.h" "*.cpp")

add_executable(main ${sources})

# Enable Profiling Macros
target_compile_definitions(main PRIVATE TRACY_ENABLE NVTX_ENABLE)

target_link_libraries(main 
PRIVATE
    Vulkan::Vulkan
    Vulkan::shaderc_combined
    SDL3::SDL3
    Eigen3
    stb_image
    assimp
    Json
    MikkTSpace
    Spirv_Reflect
    OpenEXR::OpenEXR
    Tracy::TracyClient
    NVTX::nvtx
    ws2_32 dbghelp advapi32 user32 secur32
    )
