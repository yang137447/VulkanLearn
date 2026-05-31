file(GLOB_RECURSE sources CONFIGURE_DEPENDS "*.h" "*.cpp")

add_executable(main ${sources})

target_include_directories(main PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

option(VULKANLEARN_ENABLE_TRACY "Enable Tracy profiler markers and client connection." ON)
option(VULKANLEARN_ENABLE_NVTX "Enable NVIDIA NVTX profiler markers." ON)

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
    Threads::Threads
    ws2_32 dbghelp advapi32 user32 secur32
    )

if(VULKANLEARN_ENABLE_TRACY)
    target_compile_definitions(main PRIVATE TRACY_ENABLE)
    target_link_libraries(main PRIVATE Tracy::TracyClient)
endif()

if(VULKANLEARN_ENABLE_NVTX)
    target_compile_definitions(main PRIVATE NVTX_ENABLE)
    target_link_libraries(main PRIVATE NVTX::nvtx)
endif()
