add_executable(vl_asset_edit
    main.cpp
    "${PROJECT_SOURCE_DIR}/source/editor/command/assetJsonCommandHost.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/command/editorCommand.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/command/editorCommandBus.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/command/editorCommandJsonCodec.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/command/assetDocumentFields.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/service/assetDocumentService.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/service/assetCommandExecutor.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/service/sceneDocumentService.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/persistence/materialInstancePersistence.cpp"
    "${PROJECT_SOURCE_DIR}/source/shader/build/contentHash.cpp"
    "${PROJECT_SOURCE_DIR}/source/shader/build/atomicFile.cpp"
    "${PROJECT_SOURCE_DIR}/source/mesh/validation/meshAssetValidator.cpp"
    "${PROJECT_SOURCE_DIR}/source/physics/fracture/destructibleProfile.cpp")
target_include_directories(vl_asset_edit PRIVATE "${PROJECT_SOURCE_DIR}/source")
target_link_libraries(vl_asset_edit PRIVATE Json Eigen3 Vulkan::Vulkan BLAKE3::blake3 Threads::Threads)
