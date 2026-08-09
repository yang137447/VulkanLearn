file(GLOB_RECURSE sources CONFIGURE_DEPENDS "*.h" "*.cpp")

add_executable(main ${sources})

if(TARGET copy_shaderc_dll)
    add_dependencies(main copy_shaderc_dll)
endif()

target_include_directories(main PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

option(VULKANLEARN_ENABLE_TRACY "Enable Tracy profiler markers and client connection." OFF)
option(VULKANLEARN_ENABLE_NVTX "Enable NVIDIA NVTX profiler markers." OFF)
option(VULKANLEARN_ENABLE_VULKAN_VALIDATION "Enable Vulkan validation layer and debug markers." ON)

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
    RmlUi::Core
    Threads::Threads
    ws2_32 dbghelp advapi32 user32 secur32
    )

if(VULKANLEARN_ENABLE_DEVELOPER_UI)
    target_compile_definitions(main PRIVATE VULKANLEARN_ENABLE_DEVELOPER_UI)
    target_link_libraries(main PRIVATE ImGui::ImGui)
endif()

if(VULKANLEARN_ENABLE_TRACY)
    target_compile_definitions(main PRIVATE TRACY_ENABLE)
    target_link_libraries(main PRIVATE Tracy::TracyClient)
endif()

if(VULKANLEARN_ENABLE_NVTX)
    target_compile_definitions(main PRIVATE NVTX_ENABLE)
    target_link_libraries(main PRIVATE NVTX::nvtx)
endif()

if(VULKANLEARN_ENABLE_VULKAN_VALIDATION)
    target_compile_definitions(main PRIVATE VULKANLEARN_ENABLE_VULKAN_VALIDATION)
endif()
