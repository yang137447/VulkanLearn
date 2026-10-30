file(GLOB_RECURSE sources CONFIGURE_DEPENDS "*.h" "*.cpp")
# 纯 CPU 离线核心由独立静态库拥有，不能随递归源文件再次编进 main。
list(FILTER sources EXCLUDE REGEX "/render/offline/core/")
# 纯 CPU 破碎核心由独立静态库拥有，不依赖 Vulkan/Jolt，也不重复编进 main。
list(FILTER sources EXCLUDE REGEX "/physics/")
# 动画 CPU 资产和播放器由独立库拥有，避免递归源收集重复编译。
list(FILTER sources EXCLUDE REGEX "/animation/")
list(FILTER sources EXCLUDE REGEX "/debug/")
# 可复用 UI 控件只依赖 RmlUi，不经 main 的递归收集重复编译。
list(FILTER sources EXCLUDE REGEX "/ui/widgets/")
list(FILTER sources EXCLUDE REGEX "/script/scriptHost.cpp$")
add_subdirectory(ui/widgets)

add_executable(main ${sources})

# 候选事务、组件空间与 UI 内联模板在 MinGW Debug 下超过 COFF 旧 section 上限。
if(MINGW)
    set_source_files_properties(renderSystem.cpp engine/engineLoop.cpp editor/ui/blueprintViewportPanel.cpp
        editor/ui/materialInstanceAssetEditorPanel.cpp
        PROPERTIES COMPILE_OPTIONS "-Wa,-mbig-obj")
endif()

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
    BLAKE3::blake3
    OpenEXR::OpenEXR
    RmlUi::Core
    vl_ui_widgets
    Threads::Threads
    vl_offline_core
    vl_fracture_core
    vl_animation_assets
    vl_debug_draw
    vl_script_host
    ws2_32 dbghelp advapi32 user32 secur32
    )

if(TARGET vl_physics_runtime)
    target_link_libraries(main PRIVATE vl_physics_runtime)
endif()

if(TARGET vl_gameplay)
    add_dependencies(main vl_gameplay)
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
