add_executable(vl_asset_edit
    main.cpp)
target_include_directories(vl_asset_edit PRIVATE "${PROJECT_SOURCE_DIR}/source")
target_compile_definitions(vl_asset_edit PRIVATE VL_WITH_EDITOR=1)
# 命令/服务/持久化由资产核心拥有；运行时符号（contentHash、atomicFile、
# meshAssetValidator、破碎配置）随 vl_runtime 传递进来，不能再单独编一份。
target_link_libraries(vl_asset_edit PRIVATE
    vl_editor_asset_core
    Json Eigen3 Vulkan::Vulkan BLAKE3::blake3 Threads::Threads)
