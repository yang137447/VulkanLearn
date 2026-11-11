file(GLOB_RECURSE sources CONFIGURE_DEPENDS "*.h" "*.cpp")
# 纯 CPU 离线核心由独立静态库拥有，不能随递归源文件再次编进运行时。
list(FILTER sources EXCLUDE REGEX "/render/offline/core/")
# 纯 CPU 破碎核心由独立静态库拥有，不依赖 Vulkan/Jolt，也不重复编进运行时。
list(FILTER sources EXCLUDE REGEX "/physics/")
# 动画 CPU 资产和播放器由独立库拥有，避免递归源收集重复编译。
list(FILTER sources EXCLUDE REGEX "/animation/")
list(FILTER sources EXCLUDE REGEX "/debug/")
# 发丝 CPU 导入由独立库拥有，工具和运行时共用同一份实现。
list(FILTER sources EXCLUDE REGEX "/source/hair/")
# GPU 模拟 pass 依赖 PipelineFactory 与 Vulkan backend，不能进纯 CPU 的 vl_hair_assets
# （否则 hair_asset_tests 会被迫链接 Vulkan 运行时），因此只补编进运行时。
list(APPEND sources
    "${CMAKE_CURRENT_SOURCE_DIR}/hair/hairSimulationPass.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/hair/hairSimulationPass.h"
    # 分组模拟解析依赖 TF 资产结构（含 Vulkan 顶点类型头），不能放进纯 CPU
    # 的 vl_hair_assets；模块测试单独编译同一份实现。
    "${CMAKE_CURRENT_SOURCE_DIR}/hair/tressfxGroupSimulation.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/hair/tressfxGroupSimulation.h")
# 可复用 UI 控件只依赖 RmlUi，不经运行时的递归收集重复编译。
list(FILTER sources EXCLUDE REGEX "/ui/widgets/")
list(FILTER sources EXCLUDE REGEX "/script/scriptHost.cpp$")

# 编辑器源码与编辑器专用引擎文件只允许进入 vl_editor。
list(FILTER sources EXCLUDE REGEX "/source/editor/")
list(FILTER sources EXCLUDE REGEX "/source/render/editor/")
list(FILTER sources EXCLUDE REGEX "/source/engine/editorThumbnailRenderSession\\.(h|cpp)$")
list(FILTER sources EXCLUDE REGEX "/source/engine/editorThumbnailScheduler\\.h$")
list(FILTER sources EXCLUDE REGEX "/source/ui/uiViewModelSections\\.h$")
list(FILTER sources EXCLUDE REGEX "/source/ui/uiEditorViewModelSections\\.h$")

# 入口只属于各自的可执行目标，不能进入共享静态库。
list(FILTER sources EXCLUDE REGEX "/source/main\\.cpp$")
list(FILTER sources EXCLUDE REGEX "/source/main_editor\\.cpp$")
list(FILTER sources EXCLUDE REGEX "/source/main_game\\.cpp$")

add_subdirectory(ui/widgets)

# 运行时静态库的源码不得包含编辑器头文件。链接边界只能拦住符号，头文件检查
# 在 configure 阶段就拦住反向依赖。
foreach(runtime_source IN LISTS sources)
    if(runtime_source MATCHES "\\.(h|hpp|cpp)$")
        file(READ "${runtime_source}" runtime_source_text)
        if(runtime_source_text MATCHES
            "#include[ \t]*[\"<](editor/|render/editor/|ui/widgets/editor/|ui/widgets/workspace/)")
            message(FATAL_ERROR
                "vl_runtime source '${runtime_source}' includes editor-only headers: "
                "${CMAKE_MATCH_0}")
        endif()
    endif()
endforeach()

add_library(vl_runtime STATIC ${sources})
target_include_directories(vl_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

function(vl_apply_runtime_options target)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    if(VULKANLEARN_ENABLE_TRACY)
        target_compile_definitions(${target} PRIVATE TRACY_ENABLE)
        target_link_libraries(${target} PRIVATE Tracy::TracyClient)
    endif()
    if(VULKANLEARN_ENABLE_NVTX)
        target_compile_definitions(${target} PRIVATE NVTX_ENABLE)
        target_link_libraries(${target} PRIVATE NVTX::nvtx)
    endif()
    if(VULKANLEARN_ENABLE_VULKAN_VALIDATION)
        target_compile_definitions(${target} PRIVATE VULKANLEARN_ENABLE_VULKAN_VALIDATION)
    endif()
endfunction()

function(vl_apply_executable_dependencies target)
    if(TARGET copy_shaderc_dll)
        add_dependencies(${target} copy_shaderc_dll)
    endif()
    if(TARGET vl_gameplay)
        add_dependencies(${target} vl_gameplay)
    endif()
    if(TARGET vl_prebuilt_runtime_dlls)
        add_dependencies(${target} vl_prebuilt_runtime_dlls)
    endif()
endfunction()

# 打包器只信任与二进制同目录的 flavor 标记。
function(vl_write_build_flavor target flavor editor_enabled)
    set(marker_name "vl_build_flavor.json")
    if(ARGC GREATER 3)
        set(marker_name "${ARGV3}")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DVL_FLAVOR=${flavor}
            -DVL_EDITOR_ENABLED=${editor_enabled}
            -DVL_OUTPUT=$<TARGET_FILE_DIR:${target}>/${marker_name}
            -P ${PROJECT_SOURCE_DIR}/tool/game-export/writeBuildFlavor.cmake
        VERBATIM)
endfunction()

target_link_libraries(vl_runtime
PUBLIC
    Vulkan::Vulkan
    SDL3::SDL3
    Eigen3
    Json
    RmlUi::Core
    Threads::Threads
    Vulkan::shaderc_combined
    stb_image
    assimp
    # 地形 Jolt 碰撞是核心 CPU 功能，不再只通过 C# hosting 传递。
    Jolt
    MikkTSpace
    Spirv_Reflect
    BLAKE3::blake3
    OpenEXR::OpenEXR
    vl_ui_widgets
    vl_offline_core
    vl_fracture_core
    vl_animation_assets
    vl_debug_draw
    vl_hair_assets
    vl_script_host
    ws2_32 dbghelp advapi32 user32 secur32
    )

target_link_libraries(vl_runtime PUBLIC vl_physics_runtime)
vl_apply_runtime_options(vl_runtime)

# 候选事务、组件空间、UI 内联模板与 hair trace/曲线参数在 MinGW Debug 下
# 超过 COFF 旧 section 上限。
if(MINGW)
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/renderSystem.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/engine/engineLoop.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/editor/ui/blueprintViewportPanel.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/editor/ui/materialInstanceAssetEditorPanel.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/hair/hairSimulationPass.cpp"
        PROPERTIES COMPILE_OPTIONS "-Wa,-mbig-obj")
endif()

if(VULKANLEARN_ENABLE_EDITOR)
    # 资产核心拥有不依赖 RmlUi/GPU 的命令、服务与持久化实现。它必须链接
    # vl_runtime：contentHash/atomicFile/meshAssetValidator 等符号由运行时
    # 归档提供，把同样的源再编一遍会在链接期产生重复定义。
    file(GLOB_RECURSE editor_asset_core_sources CONFIGURE_DEPENDS
        "editor/command/*.h" "editor/command/*.cpp"
        "editor/service/*.h" "editor/service/*.cpp"
        "editor/persistence/*.h" "editor/persistence/*.cpp")
    add_library(vl_editor_asset_core STATIC ${editor_asset_core_sources})
    target_include_directories(vl_editor_asset_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(vl_editor_asset_core PUBLIC vl_runtime)
    target_compile_definitions(vl_editor_asset_core PRIVATE VL_WITH_EDITOR=1)
    vl_apply_runtime_options(vl_editor_asset_core)

    file(GLOB_RECURSE editor_sources CONFIGURE_DEPENDS
        "editor/*.h" "editor/*.cpp"
        "render/editor/*.h" "render/editor/*.cpp")
    list(APPEND editor_sources
        "${CMAKE_CURRENT_SOURCE_DIR}/engine/editorThumbnailRenderSession.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/engine/editorThumbnailRenderSession.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/engine/editorThumbnailScheduler.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/ui/uiViewModelSections.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/ui/uiEditorViewModelSections.h")

    # 这三类源只属于资产核心；一旦同时留在 editor_sources 里，同一个 TU
    # 会在两个静态库中各存一份，链接时按需拉取归档成员就会撞成重复符号。
    list(FILTER editor_sources EXCLUDE REGEX "/source/editor/(command|service|persistence)/")

    # configure 阶段先兜底：资产核心不得与运行时源或编辑器源重叠。
    foreach(asset_core_source IN LISTS editor_asset_core_sources)
        list(FIND sources "${asset_core_source}" runtime_source_index)
        if(NOT runtime_source_index EQUAL -1)
            message(FATAL_ERROR
                "vl_editor_asset_core source '${asset_core_source}' is also compiled by vl_runtime")
        endif()
        list(FIND editor_sources "${asset_core_source}" editor_source_index)
        if(NOT editor_source_index EQUAL -1)
            message(FATAL_ERROR
                "vl_editor_asset_core source '${asset_core_source}' is also compiled by vl_editor")
        endif()
    endforeach()

    add_library(vl_editor STATIC ${editor_sources})
    target_include_directories(vl_editor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(vl_editor
        PUBLIC vl_runtime vl_editor_asset_core
        PRIVATE vl_editor_widgets)
    target_compile_definitions(vl_editor PRIVATE VL_WITH_EDITOR=1)
    vl_apply_runtime_options(vl_editor)

    add_executable(main_editor main_editor.cpp)
    target_link_libraries(main_editor PRIVATE vl_editor)
    vl_apply_runtime_options(main_editor)
    vl_apply_executable_dependencies(main_editor)
    vl_write_build_flavor(main_editor editor ON)
    vl_write_build_flavor(main_editor editor ON vl_build_flavor.main_editor.json)

    if(VULKANLEARN_BUILD_LEGACY_MAIN)
        # Existing scripts build `--target main`; keep the name without
        # compiling a second executable. The POST_BUILD copy produces main.exe.
        add_custom_target(main DEPENDS main_editor)
        add_custom_command(TARGET main_editor POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:main_editor>"
                "$<TARGET_FILE_DIR:main_editor>/main.exe"
            VERBATIM)
    endif()
endif()

if(VULKANLEARN_BUILD_GAME)
    add_executable(main_game main_game.cpp)
    target_link_libraries(main_game PRIVATE vl_runtime)
    vl_apply_runtime_options(main_game)
    vl_apply_executable_dependencies(main_game)
    vl_write_build_flavor(main_game game OFF vl_build_flavor.main_game.json)
    # 双 flavor 共用 bin/ 时，目录主标记跟随 main.exe 的编辑器入口；
    # 单 flavor 的游戏构建目录仍写标准 game 标记。
    if(NOT VULKANLEARN_ENABLE_EDITOR)
        vl_write_build_flavor(main_game game OFF)
    endif()
endif()
