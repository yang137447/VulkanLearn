add_executable(editor_ui_tests
    editorUiTests.cpp
    modelPreviewCameraTests.cpp
    "${PROJECT_SOURCE_DIR}/source/engine/modelPreviewCamera.cpp"
    resourceEditorControllerTests.cpp
    widgetTests.cpp
    dockingTests.cpp
    contentBrowserTests.cpp
    contentBrowserPerformanceTests.cpp
    jsonAssetLibraryTests.cpp
    thumbnailLibraryTests.cpp
    "${PROJECT_SOURCE_DIR}/source/editor/assets/editorThumbnailLibrary.cpp"
    "${PROJECT_SOURCE_DIR}/source/shader/build/contentHash.cpp"
    "${PROJECT_SOURCE_DIR}/source/shader/build/atomicFile.cpp"
    "${PROJECT_SOURCE_DIR}/source/resource/image/textureIO.cpp"
    "${PROJECT_SOURCE_DIR}/source/resource/image/hostImage.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/ui/editorContentBrowserController.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/ui/editorDockingController.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/ui/editorJsonInspectorController.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/ui/editorResourceEditorController.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/ui/editorSceneActorTree.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/assets/editorJsonAssetLibrary.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/assets/editorJsonAssetTypes.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/ui/materialInstanceAssetEditorPanel.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/command/editorCommand.cpp"
    # 逐 actor 可见性的 World/快照回归（editorUiTests.cpp）：渲染侧过滤发生在
    # WorldSnapshotBuilder，因此这里要编进真实的 World 与快照构建器，
    # 而不是用测试替身证明"标志位被读到了"。
    # meshSceneObjectIdentity.cpp 也编进来：读写两端共用同一份身份推导，
    # 测试必须证明"大纲身份"就是 World 认得的那个键。
    "${PROJECT_SOURCE_DIR}/source/core/runtimeResult.cpp"
    "${PROJECT_SOURCE_DIR}/source/world/world.cpp"
    "${PROJECT_SOURCE_DIR}/source/world/meshSceneObjectIdentity.cpp"
    "${PROJECT_SOURCE_DIR}/source/world/worldSnapshotBuilder.cpp"
    "${PROJECT_SOURCE_DIR}/source/editor/scene/editorSceneActorBuilder.cpp"
    "${PROJECT_SOURCE_DIR}/source/sceneNode.cpp")
target_include_directories(editor_ui_tests PRIVATE "${PROJECT_SOURCE_DIR}/source")
target_compile_definitions(editor_ui_tests PRIVATE
    VL_EDITOR_ASSET_ROOT="${PROJECT_SOURCE_DIR}/ui")
target_link_libraries(editor_ui_tests PRIVATE
    vl_ui_widgets RmlUi::Core Eigen3 Vulkan::Vulkan Json vl_offline_core Threads::Threads stb_image OpenEXR::OpenEXR BLAKE3::blake3)
vulkanlearn_add_gtest(editor_ui_tests)
